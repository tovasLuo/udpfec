#include "stdafx.h"
#include "FecAlgorithm.h"
#include "fec.h"

#define TRACEX_PAGE_(pFmt, ...) TRACEX_("[FecAlgorithm] " pFmt ,__VA_ARGS__)
#define TRACEX_PAGE0(pFmt, ...) TRACEX0("[FecAlgorithm] " pFmt ,__VA_ARGS__)
#define TRACEX_PAGE1(pFmt, ...) TRACEX1("[FecAlgorithm] " pFmt ,__VA_ARGS__)

fec::StreamKey get_stream_key(u32 udppwd, u16 local_port_net, u16 node_index)
{
#pragma pack(push,1)
	struct
	{
		union {
			fec::StreamKey streamKey;
			struct {
				u32 udppwd;
				u16 localPortNet;
				u16 nodeIndex;
			};
		};
	}sk;
#pragma pack(pop)

	sk.udppwd = udppwd;
	sk.localPortNet = local_port_net;
	sk.nodeIndex = node_index;

	return sk.streamKey;
}

void FecAlgorithm::WriteClose(sUdpSockInfoPtrC info)
{
	sockaddr_full tmp; tmp.family = 0;
	_channel_up.try_send(error_code{}, nullptr, nullptr, tmp, info, false);
}
void FecAlgorithm::WriteSend(sUdpPacketPtr pkt, sUdpConstBufferPtr buf_head, const sockaddr_full& ep_transer)
{
	_channel_up.try_send(error_code{}, pkt, buf_head, ep_transer, nullptr, false);
}
void FecAlgorithm::WriteRecv(sUdpPacketPtr pkt)
{
	_channel_down.try_send(error_code{}, pkt, false);
}
void FecAlgorithm::SignalExit()
{
	//�˳��źŲ��ܶ�����async_send����
	sockaddr_full tmp; tmp.family = 0;
	if (!_channel_up.try_send(error_code{}, nullptr, nullptr, tmp, nullptr, true)) {
		_channel_up.async_send(error_code{}, nullptr, nullptr, tmp, nullptr, true, as_tuple(asio::detached));
	}
	
	if (!_channel_down.try_send(error_code{}, nullptr, true)) {
		_channel_down.async_send(error_code{}, nullptr, true, as_tuple(asio::detached));
	}
}
void FecAlgorithm::send_fec_pdu_cb(fec::StreamKey streamKey, const sockaddr_full& epNode, const u8* data, u16 size, void* context)
{
	auto it = _info.find(streamKey);
	if (it == _info.end()) {
		TRACEX_PAGE_("send_fec_pdu_cb can't find streamKey: {}, epNode {}, data {}, size {}, context {}",
			streamKey, epNode, FmtPoint(data), size, FmtPoint(context));
		assert(false);
		return;
	}

	//auto pkt = std::make_shared<sUdpPacket>();
	auto pkt = sUdpPacket::Get(eUdpEventType::Send);
	pkt->info = it->second;
	pkt->node = epNode;
	pkt->remote.family = 0;

	auto buffer = const_cast<sUdpPosBuffer*>(&pkt->buffer);
	buffer->pos = 0;
	buffer->len = size;
	buffer->data.resize(size);
	memcpy((u8*)buffer->data.data(), data, size);

	_send_cb(pkt);
}
void FecAlgorithm::recv_pdu_cb(fec::StreamKey streamKey, const sockaddr_full& epNode, const sockaddr_full& epTranserLocal, const u8* data, u16 size, void* context)
{
	auto it = _info.find(streamKey);
	if (it == _info.end()) {
		return;
	}

	//auto pkt = std::make_shared<sUdpPacket>();
	auto pkt = sUdpPacket::Get(eUdpEventType::Recv);
	pkt->info = it->second;
	pkt->node = epNode;
	pkt->remote.family = 0;
	pkt->down_transer_local = epTranserLocal;

	pkt->buffer.pos = 0;
	pkt->buffer.len = size;
	pkt->buffer.data.resize(size);
	memcpy((u8*)pkt->buffer.data.data(), data, size);

	_recv_cb(pkt);
}

void FecAlgorithm::_start()
{
	co_spawn(_ctx, _coro_main(), detached);
	_th = std::thread([this]()
		{
			_ctx.run();
		});
}
void FecAlgorithm::_stop()
{
	_th.join();
}

auto FecAlgorithm::_coro_main() -> awaitable<void>
{
	TRACEX_PAGE0("_coro_main begin");

	//��ʼ��FEC ��FEC���߳��йأ�������Ҫ�������ʼ����
	//fec_env�����fec�ȳ�ʼ�������ͷ�
	fec_env fecenv;

	{
		fec fec(this);

		//����FEC����
		bool fec_poll_running = true;
		auto task_fec_poll =  co_spawn(_ctx, _coro_main_fec_poll(fec, fec_poll_running), asio::experimental::use_promise);
		
		//�������д�����
		auto task_fec_down = co_spawn(_ctx, _coro_main_down(fec), asio::experimental::use_promise);

		//�������а�
		TRACEX_PAGE0("_coro_main _channel_up recv begin");
		while (true)
		{
			auto [ec, pkt, bufHead, epTranser, closeInfo, bExit] = co_await _channel_up.async_receive(as_tuple(use_awaitable));
			if (ec) {
				TRACEX_PAGE0("_coro_main _channel_up recv error: {}", ec.message());
				//assert(false);
				//�쳣���˳�
				break;
			}

			if (bExit) [[unlikely]] {
				//�˳�
				break;
			}else if (closeInfo == nullptr) [[likely]] {
				u16 node_idx = route::get_index(pkt->node.si4.sin_addr.S_un.S_addr);
				u64 streamKey = get_stream_key(_cfg_udppwd, pkt->info->local.port_net, node_idx);
				if (streamKey == 0) [[unlikely]] {
					TRACEX_PAGE_("_coro_main: streamKey=0! udppwd={} port_net={} node_idx={} nid={}",
						_cfg_udppwd, pkt->info->local.port_net, node_idx, pkt->info->nid);
					// get_stream_key returns 0 when all inputs are 0; server mirrors this key back in downlink,
					// causing _coro_main_down to drop with streamKey2=0. Use nid as non-zero fallback.
					streamKey = pkt->info->nid != 0 ? pkt->info->nid : u64(1);
				}

				_info.try_emplace(streamKey, pkt->info);
				_streamKeys[pkt->info->nid].try_insert(streamKey);

				assert(pkt->node.family != 0);

				std::vector<fec::ref_buf> buf = {
					fec::ref_buf((u8*)bufHead->data.data(),bufHead->data.size()),
					fec::ref_buf((u8*)pkt->buffer.data.data() + pkt->buffer.pos, pkt->buffer.len)
				};
				fec.send_pdu(streamKey, epTranser, pkt->node, buf);
			}
			else [[likely]] {
				auto kv = _streamKeys.extract(closeInfo->nid);
				if (!kv.empty()) {
					auto& list = kv.mapped().values();
					for (auto& streamKey : list) {
						//ɾ��fec��·
						fec.del_fec_session(streamKey);
						//ɾ������
						_info.extract(streamKey);
					}

					TRACEX_PAGE0("_coro_main del nid {}", closeInfo->nid);
				}
			}
		}
		TRACEX_PAGE0("_coro_main _channel_up recv end");
		//�ȴ�����ֹͣ
		TRACEX_PAGE0("_coro_main wait task_fec_poll exit ...");
		fec_poll_running = false;
		co_await std::move(task_fec_poll);
		TRACEX_PAGE0("_coro_main wait task_fec_poll exit succ");
		//�ȴ����д���ֹͣ
		TRACEX_PAGE0("_coro_main wait task_fec_down exit ...");
		co_await std::move(task_fec_down);
		TRACEX_PAGE0("_coro_main wait task_fec_down exit succ");
	}

	TRACEX_PAGE0("_coro_main end");
}
auto FecAlgorithm::_coro_main_down(fec& fec) -> awaitable<void>
{
	TRACEX_PAGE0("_coro_main_down begin");

	while (true)
	{
		auto [ec, pkt, bExit] = co_await _channel_down.async_receive(as_tuple(use_awaitable));
		if (ec) {
			TRACEX_PAGE0("_coro_main_down _channel_down recv error: {}", ec.message());
			//assert(false);
			//continue;
			//�쳣���˳�
			break;
		}

		if (bExit) {
			break;
		}

		u64 streamKey2 = 0;
		auto errCode = GtpCheckPacketInvalid(pkt->buffer.data.data() + pkt->buffer.pos, pkt->buffer.len, &streamKey2);
		if (errCode == GTP_OK && streamKey2 != 0 && _streamKeys.count(pkt->info->nid)) {
			_info.try_emplace(streamKey2, pkt->info);
			_streamKeys[pkt->info->nid].try_insert(streamKey2);

			fec.recv_fec_pdu(streamKey2,
				pkt->down_transer_local, pkt->node,
				pkt->buffer.data.data() + pkt->buffer.pos, pkt->buffer.len);
		}
		else {
			// Decode GTP header fields for diagnosis: has_check_flag_ at byte 8 bit0, pack_type_ at bits 16-18 of first u32
			u8 has_check = 0, pack_type = 0;
			if (pkt->buffer.len >= 10) {
				u32 first4 = 0; u16 flags16 = 0;
				memcpy(&first4,  pkt->buffer.data.data() + pkt->buffer.pos,     sizeof(u32));
				memcpy(&flags16, pkt->buffer.data.data() + pkt->buffer.pos + 8, sizeof(u16));
				pack_type = (u8)((first4 >> 16) & 0x7);
				has_check = (u8)(flags16 & 1);
			}
			TRACEX_PAGE_("_coro_main_down drop pkt: errCode={}, streamKey2={}, nid_active={}, data_len={}, has_check={}, pack_type={}",
				errCode, streamKey2, _streamKeys.count(pkt->info->nid), pkt->buffer.len, has_check, pack_type);
		}
	}
	TRACEX_PAGE0("_coro_main_down end");
}
auto FecAlgorithm::_coro_main_fec_poll(fec& fec, bool& fec_poll_running) -> awaitable<void>
{
	TRACEX_PAGE0("_coro_main_fec_poll begin");

	steady_timer tm(co_await this_coro::executor);
	while (fec_poll_running)
	{
		tm.expires_from_now(std::chrono::milliseconds(5));
		co_await tm.async_wait(as_tuple(use_awaitable));

		if (!fec.fec_wheel()) {
			break;
		}
	}

	TRACEX_PAGE0("_coro_main_fec_poll end");

	co_return;
}