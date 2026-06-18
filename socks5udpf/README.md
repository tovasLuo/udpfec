# socks5udps

## CMake 构建

```bash
cmake -S socks5udpf -B socks5udpf/build/debug -DCMAKE_BUILD_TYPE=Debug
cmake --build socks5udpf/build/debug -j2

cmake -S socks5udpf -B socks5udpf/build/release -DCMAKE_BUILD_TYPE=Release
cmake --build socks5udpf/build/release -j2
```

输出目录：

```text
socks5udpf/bin/x64/Debug/socks5udps
socks5udpf/bin/x64/Release/socks5udps
```

Debug 链接 `socks5udpf/lib/libbitlinker64d.a`，Release 链接 `socks5udpf/lib/libbitlinker64r.a`。
