//! GT-DOS 构建期 exFAT 格式化 / 填充 / 加密工具 (Rust 版).
//!
//! 用法:
//!   gtmkfs <image> <partition-lba> <sectors> <manifest>
//!
//! manifest 每行:
//!   D <虚拟路径> <属性hex> <所有者>
//!   F <虚拟路径> <宿主文件> <属性hex> <所有者> <是否加密 0|1>
//!
//! 产物与历史 C 实现一致: exFAT 引导区 + FAT + 位图 + Up-case 表,
//! 每个条目附加 3 个 0xE0 厂商扩展项 (加密元数据 / ACL / 版本).

use gtcore::{crc32, encrypt};
use std::env;
use std::fs::{File, OpenOptions};
use std::io::{Read, Seek, SeekFrom, Write};
use std::process::exit;
use std::time::{SystemTime, UNIX_EPOCH};

const SEC: usize = 512;
const SPC: u32 = 8;
const FAT_OFF: u32 = 12;
const ROOT_CLUS: u32 = 2;
const BM_CLUS: u32 = 3;
const UP_CLUS: u32 = 4;
const DATA_CLUS: u32 = 5;
const CLUS_BYTES: u32 = SPC * SEC as u32;

const GT_XTYPE_ENCRYPT: u8 = 1;
const GT_XTYPE_ACL: u8 = 2;
const GT_XTYPE_VERSION: u8 = 3;
const GT_XFLAG_ENCRYPTED: u8 = 0x01;

const ATTR_DIR: u8 = 0x10;

#[derive(Clone)]
struct Dir {
    first: u32,
    nent: u32,
    path: String,
}

struct Mkfs {
    vol: Vec<u8>,
    fat_len: u32,
    heap_off: u32,
    cluster_count: u32,
    next_clus: u32,
    used_clus: u32,
    dirs: Vec<Dir>,
}

fn wr16(p: &mut [u8], off: usize, v: u16) {
    p[off] = (v & 0xFF) as u8;
    p[off + 1] = (v >> 8) as u8;
}

fn wr32(p: &mut [u8], off: usize, v: u32) {
    p[off..off + 4].copy_from_slice(&v.to_le_bytes());
}

fn wr64(p: &mut [u8], off: usize, v: u64) {
    p[off..off + 8].copy_from_slice(&v.to_le_bytes());
}

fn rr1_16(h: u16) -> u16 {
    (h >> 1) | (h << 15)
}

fn rr1_32(h: u32) -> u32 {
    (h >> 1) | (h << 31)
}

fn name_hash(name: &[u8]) -> u16 {
    let mut h: u16 = 0;
    for &c in name {
        let mut u = c as u16;
        if (b'a'..=b'z').contains(&c) {
            u -= 32;
        }
        h = rr1_16(h).wrapping_add(u & 0xFF);
        h = rr1_16(h).wrapping_add(u >> 8);
    }
    h
}

fn set_checksum(ents: &[u8]) -> u16 {
    let mut cs: u16 = 0;
    for (i, &b) in ents.iter().enumerate() {
        if i == 2 || i == 3 {
            continue;
        }
        cs = rr1_16(cs).wrapping_add(b as u16);
    }
    cs
}

fn table_checksum(p: &[u8]) -> u32 {
    let mut cs: u32 = 0;
    for &b in p {
        cs = rr1_32(cs).wrapping_add(b as u32);
    }
    cs
}

fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = (z - era * 146_097) as u64;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

fn now_stamp() -> u32 {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0);
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let (y, mo, d) = civil_from_days(days);
    let year = y.max(1980) as u16;
    let date = ((year - 1980) << 9) | ((mo as u16) << 5) | (d as u16);
    let hh = (rem / 3600) as u16;
    let mm = ((rem % 3600) / 60) as u16;
    let ss = (rem % 60) as u16;
    let time = (hh << 11) | (mm << 5) | (ss / 2);
    ((date as u32) << 16) | time as u32
}

fn parent_of(path: &str) -> Option<(&str, &str)> {
    let idx = path.rfind(['\\', '/']);
    match idx {
        None => Some(("", path)),
        Some(i) => {
            if path[i + 1..].is_empty() {
                return None;
            }
            Some((&path[..i], &path[i + 1..]))
        }
    }
}

impl Mkfs {
    fn new(sectors: u32) -> Option<Mkfs> {
        let mut fat_len: u32 = 64;
        let mut heap_off = 0u32;
        let mut cluster_count = 0u32;
        for _ in 0..16 {
            heap_off = FAT_OFF + fat_len;
            cluster_count = (sectors - heap_off) / SPC;
            let need = ((cluster_count as u64 + 2) * 4 + SEC as u64 - 1) / SEC as u64;
            if need <= fat_len as u64 {
                break;
            }
            fat_len = need as u32;
        }
        if cluster_count < 16 || (cluster_count + 7) / 8 > CLUS_BYTES {
            return None;
        }
        Some(Mkfs {
            vol: vec![0u8; sectors as usize * SEC],
            fat_len,
            heap_off,
            cluster_count,
            next_clus: DATA_CLUS,
            used_clus: 0,
            dirs: Vec::new(),
        })
    }

    fn clus_off(&self, c: u32) -> u64 {
        (self.heap_off as u64 + (c as u64 - 2) * SPC as u64) * SEC as u64
    }

    fn bm_off(&self, c: u32) -> usize {
        self.clus_off(BM_CLUS) as usize + (c as usize - 2) / 8
    }

    fn bm_set(&mut self, c: u32, on: bool) {
        let off = self.bm_off(c);
        let bit = 1u8 << ((c - 2) % 8);
        if on {
            self.vol[off] |= bit;
        } else {
            self.vol[off] &= !bit;
        }
    }

    fn fat_off(&self, c: u32) -> usize {
        FAT_OFF as usize * SEC + c as usize * 4
    }

    fn fget(&self, c: u32) -> u32 {
        let o = self.fat_off(c);
        u32::from_le_bytes(self.vol[o..o + 4].try_into().unwrap())
    }

    fn fset(&mut self, c: u32, v: u32) {
        let o = self.fat_off(c);
        self.vol[o..o + 4].copy_from_slice(&v.to_le_bytes());
    }

    fn alloc_chain(&mut self, nclus: u32, zero: bool) -> u32 {
        let (mut first, mut prev) = (0u32, 0u32);
        for _ in 0..nclus {
            if self.next_clus >= self.cluster_count + 2 {
                return 0;
            }
            let c = self.next_clus;
            self.next_clus += 1;
            if zero {
                let o = self.clus_off(c) as usize;
                self.vol[o..o + CLUS_BYTES as usize].fill(0);
            }
            self.bm_set(c, true);
            self.used_clus += 1;
            if first == 0 {
                first = c;
            } else {
                self.fset(prev, c);
            }
            self.fset(c, 0xFFFF_FFFF);
            prev = c;
        }
        first
    }

    fn write_cluster_data(&mut self, first: u32, data: &[u8]) {
        let mut c = first;
        let mut off = 0usize;
        while off < data.len() {
            let chunk = (data.len() - off).min(CLUS_BYTES as usize);
            let o = self.clus_off(c) as usize;
            self.vol[o..o + chunk].copy_from_slice(&data[off..off + chunk]);
            off += chunk;
            if off < data.len() {
                c = self.fget(c);
            }
        }
    }

    fn dir_find(&self, path: &str) -> Option<usize> {
        self.dirs.iter().position(|d| d.path == path)
    }

    fn dir_add(&mut self, path: &str, first: u32) -> Option<usize> {
        self.dirs.push(Dir {
            first,
            nent: 0,
            path: path.to_string(),
        });
        Some(self.dirs.len() - 1)
    }

    fn dir_capacity(&self, d: &Dir) -> u32 {
        let mut n = 0u32;
        let mut c = d.first;
        while c >= 2 && c < 0xFFFF_FFF8 {
            n += 1;
            c = self.fget(c);
        }
        n.max(1) * (CLUS_BYTES / 32)
    }

    fn dir_entry_off(&self, d: &Dir, idx: u32) -> usize {
        let per = CLUS_BYTES / 32;
        let mut c = d.first;
        for _ in 0..(idx / per) {
            c = self.fget(c);
        }
        self.clus_off(c) as usize + (idx % per) as usize * 32
    }

    fn dir_grow(&mut self, di: usize) -> bool {
        let mut c = self.dirs[di].first;
        while self.fget(c) < 0xFFFF_FFF8 {
            c = self.fget(c);
        }
        let n = self.alloc_chain(1, true);
        if n == 0 {
            return false;
        }
        self.fset(c, n);
        true
    }

    fn dir_append(&mut self, di: usize, ents: &[u8]) -> Option<u32> {
        let count = (ents.len() / 32) as u32;
        while self.dirs[di].nent + count > self.dir_capacity(&self.dirs[di].clone()) {
            if !self.dir_grow(di) {
                return None;
            }
        }
        let base = self.dirs[di].nent;
        for i in 0..count {
            let off = self.dir_entry_off(&self.dirs[di].clone(), base + i);
            self.vol[off..off + 32].copy_from_slice(&ents[i as usize * 32..(i as usize + 1) * 32]);
        }
        self.dirs[di].nent += count;
        Some(base)
    }

    /// 生成目录项集: File + Stream + Name + 3 个厂商扩展项.
    fn build_set(
        &self,
        name: &[u8],
        attr: u8,
        first: u32,
        size: u64,
        plain_size: u32,
        owner: &str,
        mode: u16,
        xflags: u8,
        crc: u32,
    ) -> Vec<u8> {
        let nlen = name.len().min(255);
        let nname = (nlen + 14) / 15;
        let secondary = 4 + nname;
        let total = 1 + secondary;
        if total > 12 {
            return Vec::new();
        }
        let mut out = vec![0u8; total * 32];
        let ts = now_stamp();

        // File (0x85)
        out[0] = 0x85;
        out[1] = secondary as u8;
        wr16(&mut out, 4, attr as u16);
        wr32(&mut out, 8, ts);
        wr32(&mut out, 12, ts);
        wr32(&mut out, 16, ts);

        // Stream Extension (0xC0)
        let s = 32;
        out[s] = 0xC0;
        out[s + 1] = if first != 0 { 0x01 } else { 0x00 };
        out[s + 3] = nlen as u8;
        wr16(&mut out, s + 4, name_hash(&name[..nlen]));
        wr64(&mut out, s + 8, size);
        wr32(&mut out, s + 20, first);
        wr64(&mut out, s + 24, size);

        // File Name (0xC1)
        for i in 0..nname {
            let e = 64 + i * 32;
            out[e] = 0xC1;
            for j in 0..15 {
                let k = i * 15 + j;
                let ch = if k < nlen { name[k] as u16 } else { 0 };
                wr16(&mut out, e + 2 + j * 2, ch);
            }
        }

        // 厂商扩展: 加密 / ACL / 版本
        let e = 64 + nname * 32;
        out[e] = 0xE0;
        out[e + 1] = GT_XTYPE_ENCRYPT;
        out[e + 4] = xflags;
        out[e + 5] = 1;
        wr32(&mut out, e + 8, plain_size);
        wr32(&mut out, e + 16, crc);

        let e = e + 32;
        out[e] = 0xE0;
        out[e + 1] = GT_XTYPE_ACL;
        wr16(&mut out, e + 4, mode);
        let ob = owner.as_bytes();
        for i in 0..ob.len().min(15) {
            out[e + 8 + i] = ob[i];
        }

        let e = e + 32;
        out[e] = 0xE0;
        out[e + 1] = GT_XTYPE_VERSION;
        wr32(&mut out, e + 4, 1);
        wr32(&mut out, e + 8, crc);

        let cs = set_checksum(&out);
        wr16(&mut out, 2, cs);
        out
    }

    fn create_dir(&mut self, path: &str, owner: &str) -> bool {
        let (parent, base) = match parent_of(path) {
            Some(v) => v,
            None => return false,
        };
        if base.is_empty() {
            return false;
        }
        let pi = match self.dir_find(parent) {
            Some(i) => i,
            None => return false,
        };
        let clus = self.alloc_chain(1, true);
        if clus == 0 {
            return false;
        }
        let ents = self.build_set(
            base.as_bytes(),
            ATTR_DIR,
            clus,
            0,
            0,
            owner,
            0,
            0,
            crc32(b""),
        );
        if ents.is_empty() || self.dir_append(pi, &ents).is_none() {
            return false;
        }
        self.dir_add(path, clus).is_some()
    }

    fn resolve_dir(&mut self, path: &str) -> Option<usize> {
        if let Some(i) = self.dir_find(path) {
            return Some(i);
        }
        // 逐级自动创建
        let norm = path.trim_start_matches(['\\', '/']);
        let mut prefix = String::new();
        let mut rest = norm;
        loop {
            let cut = rest.find(['\\', '/']);
            let (seg, tail) = match cut {
                None => break,
                Some(i) => (&rest[..i], &rest[i + 1..]),
            };
            if !prefix.is_empty() {
                prefix.push('\\');
            }
            prefix.push_str(seg);
            if self.dir_find(&prefix).is_none() && !self.create_dir(&prefix, "SYSTEM") {
                return None;
            }
            rest = tail;
        }
        self.dir_find(path)
    }

    fn add_file(
        &mut self,
        vpath: &str,
        host: &str,
        attr: u8,
        owner: &str,
        do_enc: bool,
    ) -> bool {
        let (parent, base) = match parent_of(vpath) {
            Some(v) => v,
            None => return false,
        };
        if base.is_empty() {
            return false;
        }
        let pi = match self.resolve_dir(parent) {
            Some(i) => i,
            None => return false,
        };
        let raw = match read_host_file(host) {
            Some(v) => v,
            None => return false,
        };
        let crc = crc32(&raw);
        let (data, plain_len) = if do_enc {
            let mut buf = Vec::with_capacity(raw.len() + 24);
            buf.extend_from_slice(b"GTE1");
            buf.extend_from_slice(&(raw.len() as u32).to_le_bytes());
            let mut enc = vec![0u8; (raw.len() / 16 + 1) * 16];
            let n = encrypt(&raw, &mut enc);
            buf.extend_from_slice(&enc[..n]);
            (buf, raw.len() as u32)
        } else {
            (raw.clone(), raw.len() as u32)
        };

        let nclus = if data.is_empty() {
            0
        } else {
            (data.len() as u32 + CLUS_BYTES - 1) / CLUS_BYTES
        };
        let first = if nclus > 0 { self.alloc_chain(nclus, false) } else { 0 };
        if nclus > 0 && first == 0 {
            return false;
        }
        if nclus > 0 {
            self.write_cluster_data(first, &data);
        }
        let ents = self.build_set(
            base.as_bytes(),
            attr,
            first,
            data.len() as u64,
            plain_len,
            owner,
            0,
            if do_enc { GT_XFLAG_ENCRYPTED } else { 0 },
            crc,
        );
        if ents.is_empty() {
            return false;
        }
        self.dir_append(pi, &ents).is_some()
    }

    fn init_root(&mut self) {
        // 卷标
        let mut e = [0u8; 32];
        e[0] = 0x83;
        e[1] = 6;
        for (i, c) in b"GT-DOS".iter().enumerate() {
            wr16(&mut e, 2 + i * 2, *c as u16);
        }
        let _ = self.dir_append(0, &e);

        // 分配位图
        let mut e = [0u8; 32];
        e[0] = 0x81;
        wr32(&mut e, 20, BM_CLUS);
        wr64(&mut e, 24, ((self.cluster_count + 7) / 8) as u64);
        let _ = self.dir_append(0, &e);

        // Up-case 表
        let upcase = upcase_table();
        let mut e = [0u8; 32];
        e[0] = 0x82;
        wr32(&mut e, 4, table_checksum(&upcase));
        wr32(&mut e, 20, UP_CLUS);
        wr64(&mut e, 24, upcase.len() as u64);
        let _ = self.dir_append(0, &e);
    }

    fn write_boot_sector(&mut self) {
        let volume_len = (self.vol.len() / SEC) as u64;
        let fat_len = self.fat_len;
        let heap_off = self.heap_off;
        let cluster_count = self.cluster_count;
        let used_clus = self.used_clus;
        let bs: &mut [u8] = &mut self.vol[..];
        bs[0] = 0xEB;
        bs[1] = 0x76;
        bs[2] = 0x90;
        bs[3..11].copy_from_slice(b"EXFAT   ");
        wr64(bs, 0x40, 0);
        wr64(bs, 0x48, volume_len);
        wr32(bs, 0x50, FAT_OFF);
        wr32(bs, 0x54, fat_len);
        wr32(bs, 0x58, heap_off);
        wr32(bs, 0x5C, cluster_count);
        wr32(bs, 0x60, ROOT_CLUS);
        wr32(bs, 0x64, 0x4754_4453); // "GTDS"
        wr16(bs, 0x68, 0x0100);
        wr16(bs, 0x6A, 0);
        bs[0x6C] = 9;
        bs[0x6D] = 3;
        bs[0x6E] = 1;
        bs[0x6F] = 0x80;
        bs[0x70] = (used_clus * 100 / cluster_count.max(1)) as u8;
        bs[0x1FE] = 0x55;
        bs[0x1FF] = 0xAA;

        let mut cs: u32 = 0;
        for i in 0..11 * SEC {
            if i == 106 || i == 107 || i == 112 || i == 113 {
                continue;
            }
            cs = rr1_32(cs).wrapping_add(bs[i] as u32);
        }
        for i in (0..SEC).step_by(4) {
            wr32(bs, 11 * SEC + i, cs);
        }
    }

    fn format(&mut self, manifest: &str) -> bool {
        // 系统保留簇
        for c in [ROOT_CLUS, BM_CLUS, UP_CLUS] {
            self.bm_set(c, true);
        }
        self.used_clus = 3;
        self.fset(0, 0xFFFF_FFF8);
        self.fset(1, 0xFFFF_FFFF);
        for c in [ROOT_CLUS, BM_CLUS, UP_CLUS] {
            self.fset(c, 0xFFFF_FFFF);
        }
        let upcase = upcase_table();
        let o = self.clus_off(UP_CLUS) as usize;
        self.vol[o..o + upcase.len()].copy_from_slice(&upcase);

        self.dirs.push(Dir {
            first: ROOT_CLUS,
            nent: 0,
            path: String::new(),
        });
        self.init_root();

        if !self.run_manifest(manifest) {
            return false;
        }
        self.write_boot_sector();
        true
    }

    fn run_manifest(&mut self, path: &str) -> bool {
        let text = match std::fs::read_to_string(path) {
            Ok(t) => t,
            Err(e) => {
                eprintln!("gtmkfs: cannot open manifest {} ({})", path, e);
                return false;
            }
        };
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let toks: Vec<&str> = line.split_whitespace().collect();
            if toks[0] == "D" {
                if toks.len() != 4 {
                    return false;
                }
                let (vpath, owner) = (toks[1], toks[3]);
                if self.dir_find(vpath).is_none() && !self.create_dir(vpath, owner) {
                    return false;
                }
            } else if toks[0] == "F" {
                if toks.len() != 6 {
                    return false;
                }
                let attr = match u32::from_str_radix(toks[3].trim_start_matches("0x"), 16) {
                    Ok(v) => v as u8,
                    Err(_) => return false,
                };
                let do_enc = toks[5] != "0";
                if !self.add_file(toks[1], toks[2], attr, toks[4], do_enc) {
                    return false;
                }
            }
        }
        true
    }
}

fn upcase_table() -> Vec<u8> {
    let tbl: [u16; 7] = [0x0000, 0x0061, 0x0041, 0x001A, 0x0000, 0xFF85, 0xFFFF];
    let mut out = Vec::with_capacity(tbl.len() * 2);
    for v in tbl {
        out.extend_from_slice(&v.to_le_bytes());
    }
    out
}

fn read_host_file(path: &str) -> Option<Vec<u8>> {
    let mut f = File::open(path).ok()?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf).ok()?;
    if buf.len() > (1 << 24) {
        return None;
    }
    Some(buf)
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 5 {
        eprintln!("usage: gtmkfs <image> <lba> <sectors> <manifest>");
        exit(1);
    }
    let img = &args[1];
    let lba: u64 = args[2].parse().unwrap_or(0);
    let sectors: u32 = args[3].parse().unwrap_or(0);
    let manifest = &args[4];

    let mut mk = match Mkfs::new(sectors) {
        Some(m) => m,
        None => {
            eprintln!("gtmkfs: partition size unsupported");
            exit(1);
        }
    };
    if !mk.format(manifest) {
        eprintln!("gtmkfs: manifest failed");
        exit(1);
    }
    let res = (|| -> std::io::Result<()> {
        let mut f = OpenOptions::new().read(true).write(true).open(img)?;
        f.seek(SeekFrom::Start(lba * SEC as u64))?;
        f.write_all(&mk.vol)?;
        Ok(())
    })();
    if let Err(e) = res {
        eprintln!("gtmkfs: image write failed ({})", e);
        exit(1);
    }
    println!(
        "    exFAT 卷 : {} 簇 (已用 {}), FAT {} 扇区",
        mk.cluster_count, mk.used_clus, mk.fat_len
    );
}
