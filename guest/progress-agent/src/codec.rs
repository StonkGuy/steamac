//! Base64 (standard alphabet, padded) and SHA-256 for the log bundle transfer
//! (`logs <id> <chunk>` / `logs-end <id> <sha256>`), hex for HID reports on fx.pad; no crates needed.

const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn base64(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        out.push(B64[(n >> 18) as usize & 63] as char);
        out.push(B64[(n >> 12) as usize & 63] as char);
        out.push(if c.len() > 1 { B64[(n >> 6) as usize & 63] as char } else { '=' });
        out.push(if c.len() > 2 { B64[n as usize & 63] as char } else { '=' });
    }
    out
}

/// Standard padded base64 only; reject noncanonical padding and trailing bits.
pub fn unbase64(text: &str) -> Option<Vec<u8>> {
    if text.len() % 4 != 0 { return None; }
    let mut out = Vec::with_capacity(text.len() / 4 * 3);
    for (i, chunk) in text.as_bytes().chunks(4).enumerate() {
        let digit = |b| B64.iter().position(|&c| c == b).map(|v| v as u32);
        let a = digit(chunk[0])?;
        let b = digit(chunk[1])?;
        let c = if chunk[2] == b'=' { 0 } else { digit(chunk[2])? };
        let d = if chunk[3] == b'=' { 0 } else { digit(chunk[3])? };
        let padding = if chunk[2] == b'=' { 2 } else if chunk[3] == b'=' { 1 } else { 0 };
        if padding != 0 && i + 1 != text.len() / 4 { return None; }
        if padding == 2 && (chunk[3] != b'=' || b & 15 != 0) { return None; }
        if padding == 1 && c & 3 != 0 { return None; }
        let n = a << 18 | b << 12 | c << 6 | d;
        out.push((n >> 16) as u8);
        if padding < 2 { out.push((n >> 8) as u8); }
        if padding == 0 { out.push(n as u8); }
    }
    Some(out)
}

/// Lowercase hex, two digits per byte.
pub fn hex(data: &[u8]) -> String {
    const D: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(data.len() * 2);
    for &b in data {
        out.push(D[(b >> 4) as usize] as char);
        out.push(D[(b & 15) as usize] as char);
    }
    out
}

/// `hex` back to bytes (either case); None for an odd length or a non-hex digit.
pub fn unhex(s: &str) -> Option<Vec<u8>> {
    let s = s.as_bytes();
    if s.len() % 2 != 0 {
        return None;
    }
    let digit = |c: u8| (c as char).to_digit(16).map(|d| d as u8);
    s.chunks(2).map(|p| Some(digit(p[0])? << 4 | digit(p[1])?)).collect()
}

const K: [u32; 64] = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
];

/// Lowercase hex SHA-256 of `data`.
pub fn sha256_hex(data: &[u8]) -> String {
    let mut h: [u32; 8] = [
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
    ];
    let mut msg = data.to_vec();
    let bits = (data.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    let mut w = [0u32; 64];
    for block in msg.chunks(64) {
        for (i, word) in block.chunks(4).enumerate() {
            w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16].wrapping_add(s0).wrapping_add(w[i - 7]).wrapping_add(s1);
        }
        let mut v = h;
        for i in 0..64 {
            let s1 = v[4].rotate_right(6) ^ v[4].rotate_right(11) ^ v[4].rotate_right(25);
            let ch = (v[4] & v[5]) ^ (!v[4] & v[6]);
            let t1 = v[7].wrapping_add(s1).wrapping_add(ch).wrapping_add(K[i]).wrapping_add(w[i]);
            let s0 = v[0].rotate_right(2) ^ v[0].rotate_right(13) ^ v[0].rotate_right(22);
            let maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            let t2 = s0.wrapping_add(maj);
            v = [t1.wrapping_add(t2), v[0], v[1], v[2], v[3].wrapping_add(t1), v[4], v[5], v[6]];
        }
        for (a, b) in h.iter_mut().zip(v) {
            *a = a.wrapping_add(b);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn base64_vectors() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn base64_decode_vectors() {
        for bytes in [b"".as_slice(), b"f", b"fo", b"foo", b"foobar"] {
            assert_eq!(unbase64(&base64(bytes)), Some(bytes.to_vec()));
        }
        for bad in ["Zg", "Zg==AAAA", "Zh==", "Zm9=", "=m9v", "Zg=A", "!!!!"] {
            assert_eq!(unbase64(bad), None, "{bad}");
        }
    }

    #[test]
    fn hex_round_trip() {
        assert_eq!(hex(&[0x00, 0x0f, 0xa1, 0xff]), "000fa1ff");
        assert_eq!(unhex("000fA1ff"), Some(vec![0x00, 0x0f, 0xa1, 0xff]));
        assert_eq!(unhex(""), Some(vec![]));
        assert_eq!(unhex("abc"), None, "odd length");
        assert_eq!(unhex("zz"), None);
        assert_eq!(unhex("+1"), None, "a sign is not a digit");
    }

    #[test]
    fn sha256_vectors() {
        assert_eq!(sha256_hex(b""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(sha256_hex(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        let long = vec![b'a'; 1000];
        assert_eq!(sha256_hex(&long), "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    }
}
