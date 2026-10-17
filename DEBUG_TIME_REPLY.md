# Time reply timing and interleaved mode (temporary debug branch)

Branches: `debug/time-reply-latency` here, `debug/time-interleaved` in sendspin-cpp. Not for
merging.

## What the server measures

Every `server/time` carries a trace from its stamp (`server_transmitted`) to the kernel. The
server prints a table to stderr every `SENDSPIN_DEBUG_TIME_EVERY` replies (default 200):

- The application segments: JSON, Noise, the copy into the write batch, the handoff to the network
  thread, the `send()` call and its completion.
- On Linux, `SO_TIMESTAMPING` stamps (`d` is how far the true send is past the stamp):
  - `sched`: the reply entered the qdisc.
  - `sw tx`: the driver took it.
  - `hw tx`: the NIC sent it.
- On Linux, receive stamps (`r` is how late the network thread's stamp is behind the kernel's or
  the NIC's): the software and hardware stamps of the `client/time`'s last read.
- `offset err (r-d)/2`: what the two delays do to the client's offset, in 0.1 us.

The macOS build has the application segments only.

## Interleaved mode

- **Request:** `client/time` with `"interleaved": true`.
- **Reply:** `server/time` then adds `"previous": {"client_transmitted", "server_transmitted"}`.
  This is the previous reply's actual send time: the kernel's software stamp, else sched, else
  when `send()` returned. The server sends it as soon as the software stamp is in, ahead of the
  client's next request.
- **Client:** recomputes that exchange with it. A burst sends one extra request, and only refined
  samples compete for the burst's best. If no reply carries `previous`, it falls back to the basic
  samples. It drops a refinement that moves the send by less than 0 or 100 ms or more.
- **Receive side:** on Linux the server reports `server_received` as the kernel's software
  receive stamp. Fixing only the send side would leave the offset off by r/2.
- **NIC stamps:** they go on the wire only with `SENDSPIN_DEBUG_PROTOCOL_HW=1`, since they are on
  the NIC's clock (see Hardware stamps).

## Environment

| Variable | Side | Effect |
|---|---|---|
| `SENDSPIN_DEBUG_TIME_EVERY=<n>` | server | replies per summary table |
| `SENDSPIN_DEBUG_TIME_EACH=1` | server | also one line per reply (`sN` = segment N of the table) |
| `SENDSPIN_DEBUG_APP_RX=1` | server | keep the network thread's receive stamp as `server_received` |
| `SENDSPIN_DEBUG_WANT_HW=1` | server | wait up to 10 ms for a hardware send stamp even when receives have none |
| `SENDSPIN_DEBUG_HW_SYNCED=1` | server | take the NIC's clock as the system clock (phc2sys runs) instead of estimating the offset |
| `SENDSPIN_DEBUG_PROTOCOL_HW=1` | server | put NIC stamps, converted, in `server_received` and `previous` |
| `SENDSPIN_TIME_INTERLEAVED=1` | client | ask for interleaved replies (or `SendspinClientConfig::time_interleaved`) |

## Running on a Linux box

```bash
git -C sendspin-server-cpp switch debug/time-reply-latency
git -C sendspin-cpp switch debug/time-interleaved
cmake -S sendspin-server-cpp -B build-ts -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSENDSPIN_SERVER_ENABLE_WERROR=ON -DFETCHCONTENT_SOURCE_DIR_SENDSPIN_CPP=$PWD/sendspin-cpp
cmake --build build-ts -j
SENDSPIN_DEBUG_TIME_EVERY=50 build-ts/examples/server_cli/server_cli --approve-all --log-level info /tmp/ss-state
```

On the client machine, build sendspin-cpp's `basic_client` from the debug branch and run it with
`SENDSPIN_TIME_INTERLEAVED=1`. Each burst then logs `Interleaved burst: ...`: the refined count,
the mean shift of the true send past the stamp, and the best basic and refined offsets and errors.
To check that the refined offsets scatter less, compare the two across bursts.

The in-process test players work too:

```bash
cmake -S sendspin-server-cpp -B build-ts -DSENDSPIN_SERVER_BUILD_TESTS=ON -DFETCHCONTENT_SOURCE_DIR_SENDSPIN_CPP=$PWD/sendspin-cpp
SENDSPIN_TIME_INTERLEAVED=1 build-ts/tests/sendspin_server_cpp_tests --gtest_filter='ServerPlayers*' 2>&1 | grep -E 'Interleaved burst|^\[time-reply\]|^  '
```

## Hardware stamps

- **Check the NIC:** `ethtool -T <if>` must list `hardware-transmit`, `hardware-receive` and the
  `all` receive filter. Many NICs filter receive stamps to PTP packets only, so TCP gets none.
- **Turn them on:** `sudo hwstamp_ctl -i <if> -t 1 -r 1` (linuxptp); `-r 1` asks for every
  packet.
- **Clock domain:** the stamps are in the NIC's clock, which can be anywhere (117.8 s off on one
  box). The server converts them with an estimate: the least software-minus-hardware receive
  stamp over the last 64 segments that carried both. That makes NIC times late by the driver's
  least receive latency, a few us. For exact values, run
  `sudo phc2sys -s CLOCK_REALTIME -c <if> -O 0` and set `SENDSPIN_DEBUG_HW_SYNCED=1`.
- **Wi-Fi:** drivers rarely stamp in hardware. The software send stamp is when the driver took
  the frame, before the air queue, so it misses the queueing at the access point and on the air.
