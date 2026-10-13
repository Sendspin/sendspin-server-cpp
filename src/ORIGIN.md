# Origin

Sources copied or adapted from [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) at commit `91038b72892596b9ec0cccfec1b9e6ebc58ba148`, moved into namespace `sendspin::server`.

| File | Source |
| --- | --- |
| `crypto/crypto.h` | `src/platform/crypto.h` (`Sha256`, `Sha512`, `hmac_sha512()`, `DhState`, `constant_time_equal()`; `random_bytes()` from `platform_random_bytes()`) and `src/platform/secure_zero.h` (`secure_zero`) |
| `crypto/base64url.h`, `crypto/base64url.cpp` | `src/platform/base64.h` (`b64url_encode`, `b64url_decode`), rewritten as one direct base64url codec |
| `crypto/keys.h`, `crypto/keys.cpp` | `src/crypto/keys.h`, `src/crypto/keys.cpp` and the Sentinel constants of `src/crypto/constants.h`, `src/crypto/constants.cpp`; the small-order key check and the wiping move are additions |
| `crypto/cpace.h`, `crypto/cpace.cpp`, `crypto/field25519.h` | `src/crypto/cpace.h`, `src/crypto/cpace.cpp`, `src/crypto/field25519.h`; `x25519_scalar_mult()` calls noise-c's `x25519()` directly instead of through a DH state, which computes an unused public key first, so each CPace step runs one ladder, and it masks bit 255 of the peer's point as RFC 7748 decodes a u-coordinate, so the draft's five points weak only without that mask derive as ordinary points where sendspin-cpp refuses them |
| `crypto/pairing_code.h`, `crypto/pairing_code.cpp` | `src/crypto/pairing_code.h`, `src/crypto/pairing_code.cpp`; the static-code check is left out, as the server takes a code through `code_prs()` |
| `crypto/pairing_token.h`, `crypto/pairing_token.cpp` | `src/crypto/pairing_token.h`, `src/crypto/pairing_token.cpp`; the decoders, which read a token as an operator types it, are additions |
| `crypto/psk_wrap.h`, `crypto/psk_wrap.cpp` | `src/crypto/psk_wrap.h`, `src/crypto/psk_wrap.cpp` |
| `fragmentation.h`, `fragmentation.cpp` | The reassembly and fragmentation of `src/noise_transport.cpp` (`NoiseTransport::accept_plaintext()`, `NoiseTransport::fragment_and_send()`) |

The known answers in `tests/test_pairing_crypto.cpp` come from sendspin-cpp's `tests/test_cpace.cpp`, `tests/test_pairing_code.cpp` and `tests/test_psk_wrap.cpp` at the same commit.
