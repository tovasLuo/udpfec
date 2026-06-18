#pragma once

//pkt
using fec_up_callback = std::function<void(sUdpPacketPtr pkt)>;

//ec,pkt,close
using fec_channel_up = asio::experimental::concurrent_channel<
	void(error_code, sUdpPacketPtr, sUdpConstBufferPtr, const sockaddr_full, sUdpSockInfoPtrC, bool exit)>;
using fec_channel_down = asio::experimental::concurrent_channel<void(error_code, sUdpPacketPtr, bool exit)>;

class FecAlgorithm
	: private fec::callback

{
public:
	FecAlgorithm(u32 udppwd, fec_up_callback send_cb, udp_pkt_callback recv_cb)
		: _channel_up(_ctx, 10000)
		, _channel_down(_ctx, 10000)
		, _send_cb(send_cb)
		, _recv_cb(recv_cb)
		, _cfg_udppwd(udppwd)
	{
		TRACEX0("FecAlgorithm new  begin");
		_start();
		TRACEX0("FecAlgorithm new  end");
	}
	~FecAlgorithm()
	{
		TRACEX0("FecAlgorithm free begin");
		_stop();
		TRACEX0("FecAlgorithm free end");
	}

	// 通过 IUdpNetworkProcessor 继承
	void WriteClose(sUdpSockInfoPtrC info);
	void WriteSend(sUdpPacketPtr pkt, sUdpConstBufferPtr buf_head, const sockaddr_full& ep_transer);
	void WriteRecv(sUdpPacketPtr pkt);
	void SignalExit();

private:
	virtual void send_fec_pdu_cb(fec::StreamKey streamKey, const sockaddr_full& epNode, const u8* data, u16 size, void* context) override;
	virtual void recv_pdu_cb(fec::StreamKey streamKey, const sockaddr_full& epNode, const sockaddr_full& epTranserLocal, const u8* data, u16 size, void* context) override;

private:
	void _start();
	void _stop();

	auto _coro_main() -> awaitable<void>;
	auto _coro_main_down(fec& fec) -> awaitable<void>;
	auto _coro_main_fec_poll(fec& fec, bool& fec_poll_running) -> awaitable<void>;

private:

	/* config */
	const u32 _cfg_udppwd;

	fec_up_callback		_send_cb;
	udp_pkt_callback	_recv_cb;

	/* status */
	std::map<fec::StreamKey, sUdpSockInfoPtrC>	_info;
	std::map<u64, SmallSet<fec::StreamKey, 20>>	_streamKeys;	//key=nid


	/* 外部线程 */

	/* FEC线程 */
	io_context			_ctx;
	std::thread			_th;
	fec_channel_up		_channel_up;
	fec_channel_down	_channel_down;

};

using FecAlgorithmPtr = std::shared_ptr<FecAlgorithm>;