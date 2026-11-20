//! GT-DOS 核心算法库 (Rust): AES-128-CBC + PKCS#7 + CRC32.
//!
//! 同一份代码用于两个目标:
//!   * 内核模块 (--features kernel, no_std, 通过 C ABI 导出)
//!   * 构建期工具 gtmkfs (std, 直接调用安全封装)
//!
//! 密钥与 IV 必须与历史 C 实现完全一致, 否则已加密的系统文件无法解密.

#![cfg_attr(feature = "kernel", no_std)]

// ---------------------------------------------------------------- AES 常量
const SBOX: [u8; 256] = [
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab,
    0x76, 0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4,
    0x72, 0xc0, 0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71,
    0xd8, 0x31, 0x15, 0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2,
    0xeb, 0x27, 0xb2, 0x75, 0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6,
    0xb3, 0x29, 0xe3, 0x2f, 0x84, 0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb,
    0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, 0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45,
    0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, 0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
    0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, 0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44,
    0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, 0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a,
    0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, 0xe0, 0x32, 0x3a, 0x0a, 0x49,
    0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, 0xe7, 0xc8, 0x37, 0x6d,
    0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, 0xba, 0x78, 0x25,
    0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, 0x70, 0x3e,
    0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, 0xe1,
    0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb,
    0x16,
];

const RCON: [u8; 11] = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36];

/// 128 位主密钥 (与历史实现一致)
const AES_KEY: [u8; 16] = [
    0x47, 0x54, 0x2d, 0x44, 0x4f, 0x53, 0x2d, 0x41, 0x45, 0x53, 0x2d, 0x31, 0x32, 0x38, 0x2d,
    0x4b,
];

/// CBC 初始向量
const AES_IV: [u8; 16] = [
    0x47, 0x54, 0x44, 0x4f, 0x53, 0x49, 0x56, 0x2d, 0x41, 0x45, 0x53, 0x2d, 0x43, 0x42, 0x43,
    0x00,
];

const fn make_inv_sbox() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut i = 0usize;
    while i < 256 {
        t[SBOX[i] as usize] = i as u8;
        i += 1;
    }
    t
}

const INV_SBOX: [u8; 256] = make_inv_sbox();

// ---------------------------------------------------------------- AES 轮函数
fn mul2(x: u8) -> u8 {
    (x << 1) ^ if x & 0x80 != 0 { 0x1b } else { 0x00 }
}

fn mul3(x: u8) -> u8 {
    mul2(x) ^ x
}

fn gmul(a: u8, mut b: u8) -> u8 {
    let mut p = 0u8;
    let mut a = a;
    for _ in 0..8 {
        if b & 1 != 0 {
            p ^= a;
        }
        let hi = a & 0x80;
        a <<= 1;
        if hi != 0 {
            a ^= 0x1b;
        }
        b >>= 1;
    }
    p
}

fn expand_key(key: &[u8; 16]) -> [u8; 176] {
    let mut rk = [0u8; 176];
    rk[..16].copy_from_slice(key);
    for i in 4..44 {
        let mut t = [rk[(i - 1) * 4], rk[(i - 1) * 4 + 1], rk[(i - 1) * 4 + 2], rk[(i - 1) * 4 + 3]];
        if i % 4 == 0 {
            t = [
                SBOX[t[1] as usize] ^ RCON[i / 4],
                SBOX[t[2] as usize],
                SBOX[t[3] as usize],
                SBOX[t[0] as usize],
            ];
        }
        for j in 0..4 {
            rk[i * 4 + j] = rk[(i - 4) * 4 + j] ^ t[j];
        }
    }
    rk
}

fn add_round_key(s: &mut [u8; 16], rk: &[u8; 176], round: usize) {
    for i in 0..16 {
        s[i] ^= rk[round * 16 + i];
    }
}

fn shift_rows(s: &mut [u8; 16]) {
    let t = *s;
    s[1] = t[5];
    s[5] = t[9];
    s[9] = t[13];
    s[13] = t[1];
    s[2] = t[10];
    s[6] = t[14];
    s[10] = t[2];
    s[14] = t[6];
    s[3] = t[15];
    s[7] = t[3];
    s[11] = t[7];
    s[15] = t[11];
}

fn inv_shift_rows(s: &mut [u8; 16]) {
    let t = *s;
    s[1] = t[13];
    s[5] = t[1];
    s[9] = t[5];
    s[13] = t[9];
    s[2] = t[10];
    s[6] = t[14];
    s[10] = t[2];
    s[14] = t[6];
    s[3] = t[7];
    s[7] = t[11];
    s[11] = t[15];
    s[15] = t[3];
}

fn mix_columns(s: &mut [u8; 16]) {
    for c in 0..4 {
        let i = 4 * c;
        let a = [s[i], s[i + 1], s[i + 2], s[i + 3]];
        s[i] = mul2(a[0]) ^ mul3(a[1]) ^ a[2] ^ a[3];
        s[i + 1] = a[0] ^ mul2(a[1]) ^ mul3(a[2]) ^ a[3];
        s[i + 2] = a[0] ^ a[1] ^ mul2(a[2]) ^ mul3(a[3]);
        s[i + 3] = mul3(a[0]) ^ a[1] ^ a[2] ^ mul2(a[3]);
    }
}

fn inv_mix_columns(s: &mut [u8; 16]) {
    for c in 0..4 {
        let i = 4 * c;
        let a = [s[i], s[i + 1], s[i + 2], s[i + 3]];
        s[i] = gmul(a[0], 14) ^ gmul(a[1], 11) ^ gmul(a[2], 13) ^ gmul(a[3], 9);
        s[i + 1] = gmul(a[0], 9) ^ gmul(a[1], 14) ^ gmul(a[2], 11) ^ gmul(a[3], 13);
        s[i + 2] = gmul(a[0], 13) ^ gmul(a[1], 9) ^ gmul(a[2], 14) ^ gmul(a[3], 11);
        s[i + 3] = gmul(a[0], 11) ^ gmul(a[1], 13) ^ gmul(a[2], 9) ^ gmul(a[3], 14);
    }
}

fn encrypt_block(rk: &[u8; 176], input: &[u8; 16]) -> [u8; 16] {
    let mut s = *input;
    add_round_key(&mut s, rk, 0);
    for r in 1..10 {
        for x in s.iter_mut() {
            *x = SBOX[*x as usize];
        }
        shift_rows(&mut s);
        mix_columns(&mut s);
        add_round_key(&mut s, rk, r);
    }
    for x in s.iter_mut() {
        *x = SBOX[*x as usize];
    }
    shift_rows(&mut s);
    add_round_key(&mut s, rk, 10);
    s
}

fn decrypt_block(rk: &[u8; 176], input: &[u8; 16]) -> [u8; 16] {
    let mut s = *input;
    add_round_key(&mut s, rk, 10);
    for r in (1..10).rev() {
        inv_shift_rows(&mut s);
        for x in s.iter_mut() {
            *x = INV_SBOX[*x as usize];
        }
        add_round_key(&mut s, rk, r);
        inv_mix_columns(&mut s);
    }
    inv_shift_rows(&mut s);
    for x in s.iter_mut() {
        *x = INV_SBOX[*x as usize];
    }
    add_round_key(&mut s, rk, 0);
    s
}

// ------------------------------------------------------------ 安全封装 (工具用)
/// AES-128-CBC + PKCS#7 加密, 写入 out, 返回密文长度 (16 的倍数).
/// `out` 至少要有 `(data.len() / 16 + 1) * 16` 字节.
pub fn encrypt(data: &[u8], out: &mut [u8]) -> usize {
    let rk = expand_key(&AES_KEY);
    let pad = 16 - (data.len() % 16);
    let total = data.len() + pad;
    assert!(out.len() >= total);
    let mut prev = AES_IV;
    let mut off = 0;
    while off < total {
        let mut blk = [0u8; 16];
        for i in 0..16 {
            let m = if off + i < data.len() {
                data[off + i]
            } else {
                pad as u8
            };
            blk[i] = m ^ prev[i];
        }
        blk = encrypt_block(&rk, &blk);
        out[off..off + 16].copy_from_slice(&blk);
        prev = blk;
        off += 16;
    }
    total
}

/// CBC 解密并去除填充; 失败返回 None.
pub fn decrypt(data: &[u8], out: &mut [u8]) -> Option<usize> {
    if data.is_empty() || data.len() % 16 != 0 || out.len() < data.len() {
        return None;
    }
    let rk = expand_key(&AES_KEY);
    let mut prev = AES_IV;
    let mut off = 0;
    while off < data.len() {
        let mut inb = [0u8; 16];
        inb.copy_from_slice(&data[off..off + 16]);
        let blk = decrypt_block(&rk, &inb);
        for i in 0..16 {
            out[off + i] = blk[i] ^ prev[i];
        }
        prev = inb;
        off += 16;
    }
    let pad = out[data.len() - 1] as usize;
    if pad == 0 || pad > 16 || pad > data.len() {
        return None;
    }
    for i in 0..pad {
        if out[data.len() - 1 - i] as usize != pad {
            return None;
        }
    }
    Some(data.len() - pad)
}

/// IEEE CRC32 (与历史实现一致).
pub fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xFFFF_FFFFu32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320u32 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

// ------------------------------------------------------------ C ABI (内核)
// 注意: 内核里的解密调用是"原地"完成的 (out 与 input 可能重叠 8 字节),
// 因此这里逐块复制, 不构造互相别名的切片 (那会是 UB).

#[no_mangle]
pub unsafe extern "C" fn gt_crc32(data: *const u8, len: u32) -> u32 {
    if len == 0 {
        return crc32(&[]);
    }
    if data.is_null() {
        return 0;
    }
    crc32(core::slice::from_raw_parts(data, len as usize))
}

#[no_mangle]
pub unsafe extern "C" fn gt_encrypt(input: *const u8, len: u32, out: *mut u8) -> u32 {
    if input.is_null() || out.is_null() {
        return 0;
    }
    let rk = expand_key(&AES_KEY);
    let pad = 16 - (len as usize % 16);
    let total = len as usize + pad;
    let mut prev = AES_IV;
    let mut off = 0usize;
    while off < total {
        let mut blk = [0u8; 16];
        for i in 0..16 {
            let m = if off + i < len as usize {
                *input.add(off + i)
            } else {
                pad as u8
            };
            blk[i] = m ^ prev[i];
        }
        let ct = encrypt_block(&rk, &blk);
        core::ptr::copy_nonoverlapping(ct.as_ptr(), out.add(off), 16);
        prev = ct;
        off += 16;
    }
    total as u32
}

#[no_mangle]
pub unsafe extern "C" fn gt_decrypt(input: *const u8, clen: u32, out: *mut u8) -> u32 {
    if input.is_null() || out.is_null() || clen == 0 || clen % 16 != 0 {
        return 0;
    }
    let rk = expand_key(&AES_KEY);
    let total = clen as usize;
    let mut prev = AES_IV;
    let mut off = 0usize;
    while off < total {
        let mut inb = [0u8; 16];
        core::ptr::copy_nonoverlapping(input.add(off), inb.as_mut_ptr(), 16);
        let blk = decrypt_block(&rk, &inb);
        let mut pt = [0u8; 16];
        for i in 0..16 {
            pt[i] = blk[i] ^ prev[i];
        }
        core::ptr::copy_nonoverlapping(pt.as_ptr(), out.add(off), 16);
        prev = inb;
        off += 16;
    }
    let pad = *out.add(total - 1) as usize;
    if pad == 0 || pad > 16 || pad > total {
        return 0;
    }
    for i in 0..pad {
        if *out.add(total - 1 - i) as usize != pad {
            return 0;
        }
    }
    (total - pad) as u32
}

#[cfg(feature = "kernel")]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {
        unsafe {
            core::arch::asm!("cli; hlt", options(nomem, nostack, preserves_flags));
        }
    }
}

// ---------------------------------------------------------------- 测试
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn aes_fips197_vector() {
        // FIPS-197 附录 C.1
        let mut key = [0u8; 16];
        for i in 0..16 {
            key[i] = i as u8;
        }
        let rk = expand_key(&key);
        let mut pt = [0u8; 16];
        for i in 0..16 {
            pt[i] = (i * 0x11) as u8;
        }
        let ct = encrypt_block(&rk, &pt);
        let expect = [
            0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4,
            0xc5, 0x5a,
        ];
        assert_eq!(ct, expect);
        assert_eq!(decrypt_block(&rk, &ct), pt);
    }

    #[test]
    fn cbc_roundtrip() {
        let data = b"GT-DOS rust gtcore round-trip payload";
        let mut enc = [0u8; 64];
        let n = encrypt(data, &mut enc);
        assert_eq!(n, 48);
        let mut dec = [0u8; 64];
        let m = decrypt(&enc[..n], &mut dec).unwrap();
        assert_eq!(&dec[..m], data);
    }

    #[test]
    fn crc32_vector() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn inverse_ops() {
        let mut s = [0u8; 16];
        for i in 0..16 {
            s[i] = (i * 7 + 3) as u8;
        }
        let mut a = s;
        shift_rows(&mut a);
        inv_shift_rows(&mut a);
        assert_eq!(a, s);

        let mut b = s;
        mix_columns(&mut b);
        inv_mix_columns(&mut b);
        assert_eq!(b, s);

        assert!((0..256).all(|x| INV_SBOX[SBOX[x] as usize] as usize == x));
    }

    #[test]
    fn abi_overlapping_decrypt() {
        // 复现内核 fat_read_file: GTE1 + 明文长 + 密文, 解密写回同一缓冲
        let data = b"GT-DOS service payload for overlapping decrypt";
        let mut packed = vec![0u8; 1024];
        packed[..4].copy_from_slice(b"GTE1");
        packed[4..8].copy_from_slice(&(data.len() as u32).to_le_bytes());
        let n = unsafe {
            gt_encrypt(data.as_ptr(), data.len() as u32, packed.as_mut_ptr().add(8))
        };
        assert_eq!(n as usize % 16, 0);
        let d = unsafe {
            gt_decrypt(packed.as_ptr().add(8), n, packed.as_mut_ptr())
        };
        assert_eq!(d as usize, data.len());
        assert_eq!(&packed[..data.len()], data);
    }
}
