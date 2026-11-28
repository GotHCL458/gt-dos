//! GT-DOS 格式解析器 (no_std, C ABI).
//!
//!   * `.gtl` 共享库: 头部校验与导出表解析
//!   * `.gtd` 字模: 码点表二分查找 (每个字模 128 字节)
//!   * `.gtp` 程序: 明文 CRC32 完整性校验
//!   * exFAT 目录项集: 0x85 / 0xC0 / 0xC1 / 0xE0
//!
//! C 侧只保留内存映射/跳转与缓冲区管理, 所有格式判断都在这里完成.

use crate::crc32;
use core::ptr;

const MAX_EXPORTS: usize = 32;
const MAX_SET_ENTS: u32 = 12;
const GLYPH_BYTES: usize = 128;
const NAME_MAX: usize = 63;

// ------------------------------------------------------------------ 小端读取
fn rd16(p: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([p[o], p[o + 1]])
}

fn rd32(p: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([p[o], p[o + 1], p[o + 2], p[o + 3]])
}

fn rd64(p: &[u8], o: usize) -> u64 {
    let lo = rd32(p, o) as u64;
    let hi = rd32(p, o + 4) as u64;
    lo | (hi << 32)
}

// ============================================================ .gtl 共享库
#[repr(C)]
#[derive(Clone, Copy)]
pub struct GtGtlExport {
    pub name: [u8; 24],
    pub rva: u32,
}

impl GtGtlExport {
    const ZERO: GtGtlExport = GtGtlExport {
        name: [0; 24],
        rva: 0,
    };
}

#[repr(C)]
pub struct GtGtlInfo {
    pub version: u32,
    pub image_size: u32,
    pub image_off: u32,
    pub count: u32,
    pub exports: [GtGtlExport; MAX_EXPORTS],
    /// 可选内嵌图标: "GTI1" + u16 w + u16 h + RGBA, 紧跟镜像之后.
    pub icon_off: u32,
    pub icon_len: u32,
    pub icon_w: u32,
    pub icon_h: u32,
}

/// 解析 .gtl 头部. 返回 0 成功; image_off 为镜像在文件内的起始偏移.
#[no_mangle]
pub unsafe extern "C" fn gt_gtl_parse(
    buf: *const u8,
    len: u32,
    out: *mut GtGtlInfo,
) -> i32 {
    if buf.is_null() || out.is_null() || len < 16 {
        return -1;
    }
    let b = core::slice::from_raw_parts(buf, len as usize);
    if &b[0..4] != b"GTL1" {
        return -1;
    }
    let version = rd32(b, 4);
    let image_size = rd32(b, 8);
    let count = rd32(b, 12);
    if count > MAX_EXPORTS as u32 {
        return -1;
    }
    let image_off = 16u32 + 28 * count;
    if image_size == 0 || image_off as u64 + image_size as u64 > len as u64 {
        return -1;
    }

    let info = &mut *out;
    info.version = version;
    info.image_size = image_size;
    info.image_off = image_off;
    info.count = count;
    info.icon_off = 0;
    info.icon_len = 0;
    info.icon_w = 0;
    info.icon_h = 0;
    for i in 0..MAX_EXPORTS {
        info.exports[i] = GtGtlExport::ZERO;
    }
    for i in 0..count as usize {
        let base = 16 + 28 * i;
        let e = &b[base..base + 28];
        let name = info.exports[i].name.as_mut();
        let mut k = 0;
        while k < 23 && e[k] != 0 {
            name[k] = e[k];
            k += 1;
        }
        name[k] = 0;
        info.exports[i].rva = rd32(e, 24);
    }

    // 可选内嵌图标块: 紧跟镜像; 无魔数表示无图标 (向后兼容).
    let icon_off = image_off as usize + image_size as usize;
    if len as usize >= icon_off + 8 && &b[icon_off..icon_off + 4] == b"GTI1" {
        let iw = rd16(b, icon_off + 4) as u32;
        let ih = rd16(b, icon_off + 6) as u32;
        if iw == 0 || ih == 0 || iw > 256 || ih > 256 {
            return -1;
        }
        let need = 8usize + (iw as usize) * (ih as usize) * 4;
        if icon_off + need > len as usize {
            return -1;
        }
        info.icon_off = icon_off as u32;
        info.icon_len = need as u32;
        info.icon_w = iw;
        info.icon_h = ih;
    }
    0
}

// ============================================================ .gtd 字模
/// 码点表二分查找; 命中返回字模指针 (128 字节), 否则 NULL.
#[no_mangle]
pub unsafe extern "C" fn gt_gtd_lookup(
    cpm: *const u16,
    count: u32,
    font: *const u8,
    cp: u32,
) -> *const u8 {
    if cpm.is_null() || font.is_null() || count == 0 || cp > 0xFFFF {
        return ptr::null();
    }
    let tab = core::slice::from_raw_parts(cpm, count as usize);
    let mut lo = 0usize;
    let mut hi = count as usize;
    while lo < hi {
        let mid = (lo + hi) / 2;
        let v = tab[mid] as u32;
        if v == cp {
            return font.add(mid * GLYPH_BYTES);
        }
        if v < cp {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    ptr::null()
}

// ============================================================ .gtp 完整性
/// 校验已载入的 .gtp 明文 CRC32. 0=通过, -1=参数错, -2=CRC 不符.
#[no_mangle]
pub unsafe extern "C" fn gt_gtp_verify(buf: *const u8, len: u32, expect: u32) -> i32 {
    if buf.is_null() || len == 0 {
        return -1;
    }
    if crc32(core::slice::from_raw_parts(buf, len as usize)) != expect {
        return -2;
    }
    0
}

// ====================================================== exFAT 目录项集
#[repr(C)]
pub struct GtExfatEntry {
    pub attr: u16,
    pub no_fat: u8,
    pub xflags: u8,
    pub count: u32,
    pub size: u64,
    pub first: u32,
    pub plain_size: u32,
    pub crc: u32,
    pub version: u32,
    pub mode: u16,
    pub nlen: u16,
    pub owner: [u8; 16],
    pub name: [u8; 64],
}

const DEFAULT_OWNER: [u8; 16] = *b"SYSTEM\0\0\0\0\0\0\0\0\0\0";

fn rr1_16(h: u16) -> u16 {
    (h >> 1) | (h << 15)
}

fn set_checksum(ents: &[u8], count: usize) -> u16 {
    let mut cs: u16 = 0;
    for (i, &b) in ents[..count * 32].iter().enumerate() {
        if i == 2 || i == 3 {
            continue;
        }
        cs = rr1_16(cs).wrapping_add(b as u16);
    }
    cs
}

/// 解析一个 exFAT 文件目录项集 (起始项 0x85).
/// 返回 0 成功; -1 参数/类型, -2 项数, -3 越界, -4 校验和, -5 缺流项, -6 空名.
#[no_mangle]
pub unsafe extern "C" fn gt_exfat_parse(
    ents: *const u8,
    avail: u32,
    out: *mut GtExfatEntry,
) -> i32 {
    if ents.is_null() || out.is_null() || avail == 0 {
        return -1;
    }
    let e = core::slice::from_raw_parts(ents, avail as usize * 32);
    if e[0] != 0x85 {
        return -1;
    }
    let count = e[1] as u32;
    if count < 2 || count > MAX_SET_ENTS - 1 {
        return -2;
    }
    if 1 + count > avail {
        return -3;
    }
    let total = (1 + count) as usize;
    if set_checksum(e, total) != rd16(e, 2) {
        return -4;
    }

    let o = &mut *out;
    ptr::write_bytes(o as *mut GtExfatEntry as *mut u8, 0, core::mem::size_of::<GtExfatEntry>());
    o.attr = rd16(e, 4);
    o.count = total as u32;
    o.version = 1;
    o.owner = DEFAULT_OWNER;

    let mut stream = false;
    let mut nlen = 0usize;
    for i in 1..=count as usize {
        let s = &e[i * 32..i * 32 + 32];
        if s[0] == 0xC0 {
            stream = true;
            nlen = s[3] as usize;
            o.no_fat = if s[1] & 0x02 != 0 { 1 } else { 0 };
            o.size = rd64(s, 8);
            o.first = rd32(s, 20);
            let sz2 = rd64(s, 24);
            if sz2 > o.size {
                o.size = sz2;
            }
        }
    }
    if !stream {
        return -5;
    }

    let mut n = 0usize;
    for i in 1..=count as usize {
        if n >= nlen {
            break;
        }
        let s = &e[i * 32..i * 32 + 32];
        if s[0] != 0xC1 {
            continue;
        }
        for j in 0..15 {
            if n >= nlen || n >= NAME_MAX {
                break;
            }
            let ch = rd16(s, 2 + j * 2);
            o.name[n] = if ch != 0 && ch < 128 { ch as u8 } else { b'?' };
            n += 1;
        }
    }
    o.name[n] = 0;
    o.nlen = n as u16;
    if n == 0 {
        return -6;
    }

    o.plain_size = if o.size > 0xFFFF_FFFF {
        0xFFFF_FFFF
    } else {
        o.size as u32
    };
    for i in 1..=count as usize {
        let s = &e[i * 32..i * 32 + 32];
        if s[0] != 0xE0 {
            continue;
        }
        match s[1] {
            1 => {
                o.xflags = s[4];
                o.plain_size = rd32(s, 8);
                o.crc = rd32(s, 16);
            }
            2 => {
                o.mode = rd16(s, 4);
                o.owner.copy_from_slice(&s[8..24]);
                o.owner[15] = 0;
            }
            3 => {
                o.version = rd32(s, 4);
                o.crc = rd32(s, 8);
            }
            _ => {}
        }
    }
    if o.owner[0] == 0 {
        o.owner = DEFAULT_OWNER;
    }
    0
}

// -------------------------------------------------------------------- 测试
#[cfg(test)]
mod tests {
    use super::*;

    fn build_gtl() -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(b"GTL1");
        v.extend_from_slice(&1u32.to_le_bytes());
        v.extend_from_slice(&4u32.to_le_bytes());
        v.extend_from_slice(&2u32.to_le_bytes());
        let mut e = [0u8; 24];
        e[..3].copy_from_slice(b"add");
        v.extend_from_slice(&e);
        v.extend_from_slice(&0u32.to_le_bytes());
        let mut e = [0u8; 24];
        e[..3].copy_from_slice(b"mul");
        v.extend_from_slice(&e);
        v.extend_from_slice(&2u32.to_le_bytes());
        v.extend_from_slice(&[0x90, 0x90, 0x90, 0xC3]);
        v
    }

    #[test]
    fn gtl_parse() {
        let f = build_gtl();
        let mut info = unsafe { core::mem::zeroed::<GtGtlInfo>() };
        let rc = unsafe { gt_gtl_parse(f.as_ptr(), f.len() as u32, &mut info) };
        assert_eq!(rc, 0);
        assert_eq!(info.count, 2);
        assert_eq!(info.image_off, 16 + 28 * 2);
        assert_eq!(&info.exports[0].name[..3], b"add");
        assert_eq!(info.exports[1].rva, 2);
        // 截断后必须拒绝
        let rc = unsafe { gt_gtl_parse(f.as_ptr(), (f.len() - 1) as u32, &mut info) };
        assert_ne!(rc, 0);
    }

    #[test]
    fn gtl_icon() {
        let mut f = build_gtl();
        let mut info = unsafe { core::mem::zeroed::<GtGtlInfo>() };
        let rc = unsafe { gt_gtl_parse(f.as_ptr(), f.len() as u32, &mut info) };
        assert_eq!(rc, 0);
        assert_eq!(info.icon_len, 0);

        // 追加 2x2 RGBA 图标块
        let px = [1u8, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255];
        f.extend_from_slice(b"GTI1");
        f.extend_from_slice(&2u16.to_le_bytes());
        f.extend_from_slice(&2u16.to_le_bytes());
        f.extend_from_slice(&px);
        let rc = unsafe { gt_gtl_parse(f.as_ptr(), f.len() as u32, &mut info) };
        assert_eq!(rc, 0);
        assert_eq!(info.icon_w, 2);
        assert_eq!(info.icon_h, 2);
        assert_eq!(info.icon_len, 8 + 16);
        assert_eq!(info.icon_off as usize, 16 + 28 * 2 + 4);

        // 图标块截断 -> 拒绝
        let rc = unsafe { gt_gtl_parse(f.as_ptr(), (f.len() - 1) as u32, &mut info) };
        assert_ne!(rc, 0);
    }

    fn build_set(name: &[u8], payload: &[u8], crc: u32) -> Vec<u8> {
        let nlen = name.len();
        let nname = (nlen + 14) / 15;
        let total = 1 + 4 + nname;
        let mut v = vec![0u8; total * 32];
        v[0] = 0x85;
        v[1] = (4 + nname) as u8;
        v[4] = 0x20;
        let s = 32;
        v[s] = 0xC0;
        v[s + 1] = 0x01;
        v[s + 3] = nlen as u8;
        v[s + 8..s + 16].copy_from_slice(&(payload.len() as u64).to_le_bytes());
        v[s + 20..s + 24].copy_from_slice(&5u32.to_le_bytes());
        v[s + 24..s + 32].copy_from_slice(&(payload.len() as u64).to_le_bytes());
        for i in 0..nname {
            let e = 64 + i * 32;
            v[e] = 0xC1;
            for j in 0..15 {
                let k = i * 15 + j;
                if k < nlen {
                    let off = e + 2 + j * 2;
                    v[off] = name[k];
                }
            }
        }
        let e = 64 + nname * 32;
        v[e] = 0xE0;
        v[e + 1] = 1;
        v[e + 8..e + 12].copy_from_slice(&(payload.len() as u32).to_le_bytes());
        v[e + 16..e + 20].copy_from_slice(&crc.to_le_bytes());
        let e = e + 32;
        v[e] = 0xE0;
        v[e + 1] = 2;
        v[e + 8..e + 14].copy_from_slice(b"SYSTEM");
        let e = e + 32;
        v[e] = 0xE0;
        v[e + 1] = 3;
        v[e + 4..e + 8].copy_from_slice(&1u32.to_le_bytes());
        v[e + 8..e + 12].copy_from_slice(&crc.to_le_bytes());
        let cs = set_checksum(&v, total);
        v[2..4].copy_from_slice(&cs.to_le_bytes());
        v
    }

    #[test]
    fn exfat_parse() {
        let data = b"GT-DOS dir entry";
        let crc = crc32(data);
        let mut set = build_set(b"HELLO.GTP", data, crc);
        set.resize(12 * 32, 0);
        let mut e = unsafe { core::mem::zeroed::<GtExfatEntry>() };
        let rc = unsafe { gt_exfat_parse(set.as_ptr(), 12, &mut e) };
        assert_eq!(rc, 0);
        assert_eq!(&e.name[..9], b"HELLO.GTP");
        assert_eq!(e.count, 6);
        assert_eq!(e.size, data.len() as u64);
        assert_eq!(e.first, 5);
        assert_eq!(e.crc, crc);
        assert_eq!(&e.owner[..6], b"SYSTEM");

        // 破坏校验和 -> 拒绝
        set[4] ^= 0xFF;
        let rc = unsafe { gt_exfat_parse(set.as_ptr(), 12, &mut e) };
        assert_eq!(rc, -4);
    }

    #[test]
    fn gtp_verify() {
        let data = b"program bytes";
        assert_eq!(unsafe { gt_gtp_verify(data.as_ptr(), data.len() as u32, crc32(data)) }, 0);
        assert_eq!(unsafe { gt_gtp_verify(data.as_ptr(), data.len() as u32, 1) }, -2);
    }

    #[test]
    fn gtd_lookup() {
        let cpm: Vec<u16> = vec![0x20, 0x41, 0x4E2D, 0xFF01];
        let font = vec![0u8; cpm.len() * 128];
        let p = unsafe { gt_gtd_lookup(cpm.as_ptr(), cpm.len() as u32, font.as_ptr(), 0x4E2D) };
        assert_eq!(p as usize, font.as_ptr() as usize + 2 * 128);
        let p = unsafe { gt_gtd_lookup(cpm.as_ptr(), cpm.len() as u32, font.as_ptr(), 0x99) };
        assert!(p.is_null());
    }
}
