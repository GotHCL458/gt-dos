//! GT-DOS 字模生成器 (Rust 版, 替换 tools/mkfont.py).
//!
//! 用 Windows GDI 从系统 TrueType 字体提取 16x16 灰度字模:
//!   * 原生 16px 渲染 + CLEARTYPE_QUALITY (hinting + 灰度边缘)
//!   * 4bit 灰度, 基线按 TEXTMETRIC.tmAscent 自适应, 中英文共用基线
//!
//! 输出 (默认目录 build/):
//!   font.gtd  每字模 128 字节 (16 行 x 8 字节, 4bit/像素, 行内 MSB first)
//!   cpm.gtd   码点表, u16 小端升序

use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::exit;

#[cfg(not(windows))]
fn main() {
    eprintln!("[!] 本工具需要 Windows GDI");
    exit(1);
}

#[cfg(windows)]
mod win {
    use std::ffi::c_void;

    pub type Handle = *mut c_void;

    #[repr(C)]
    #[allow(non_snake_case)]
    pub struct BitmapInfoHeader {
        pub biSize: u32,
        pub biWidth: i32,
        pub biHeight: i32,
        pub biPlanes: u16,
        pub biBitCount: u16,
        pub biCompression: u32,
        pub biSizeImage: u32,
        pub biXPelsPerMeter: i32,
        pub biYPelsPerMeter: i32,
        pub biClrUsed: u32,
        pub biClrImportant: u32,
    }

    #[link(name = "gdi32")]
    extern "system" {
        pub fn CreateCompatibleDC(hdc: Handle) -> Handle;
        pub fn CreateDIBSection(
            hdc: Handle,
            bmi: *const BitmapInfoHeader,
            usage: u32,
            bits: *mut *mut c_void,
            section: Handle,
            offset: u32,
        ) -> Handle;
        pub fn SelectObject(hdc: Handle, obj: Handle) -> Handle;
        pub fn CreateFontW(
            height: i32,
            width: i32,
            escapement: i32,
            orientation: i32,
            weight: i32,
            italic: u32,
            underline: u32,
            strike_out: u32,
            char_set: u32,
            out_precision: u32,
            clip_precision: u32,
            quality: u32,
            pitch_and_family: u32,
            face: *const u16,
        ) -> Handle;
        pub fn PatBlt(hdc: Handle, x: i32, y: i32, w: i32, h: i32, rop: u32) -> i32;
        pub fn SetBkColor(hdc: Handle, color: u32) -> u32;
        pub fn SetTextColor(hdc: Handle, color: u32) -> u32;
        pub fn SetTextAlign(hdc: Handle, align: u32) -> u32;
        pub fn ExtTextOutW(
            hdc: Handle,
            x: i32,
            y: i32,
            options: u32,
            rect: *const c_void,
            text: *const u16,
            count: u32,
            dx: *const i32,
        ) -> i32;
        pub fn GetTextMetricsW(hdc: Handle, tm: *mut u8) -> i32;
        pub fn DeleteObject(obj: Handle) -> i32;
        pub fn DeleteDC(hdc: Handle) -> i32;
    }

    pub const BLACKNESS: u32 = 0x0000_0042;
    pub const DIB_RGB_COLORS: u32 = 0;
    pub const CLEARTYPE_QUALITY: u32 = 5;
    pub const DEFAULT_CHARSET: u32 = 134;
    pub const PITCH_FAMILY: u32 = 0x21; // FF_SWISS | FIXED_PITCH (与历史脚本一致)
}

#[cfg(windows)]
const CELL: usize = 16;
#[cfg(windows)]
const DIB: usize = 32;
#[cfg(windows)]
const BYTES_PER_GLYPH: usize = CELL * 8;
#[cfg(windows)]
const FONT_EM: i32 = -16;
#[cfg(windows)]
const RENDER_X: i32 = 6;
#[cfg(windows)]
const RENDER_Y: i32 = 8;

#[cfg(windows)]
struct Renderer {
    dc: win::Handle,
    bitmap: win::Handle,
    bits: *mut u8,
}

#[cfg(windows)]
impl Renderer {
    fn new() -> Option<Renderer> {
        let bi = win::BitmapInfoHeader {
            biSize: std::mem::size_of::<win::BitmapInfoHeader>() as u32,
            biWidth: DIB as i32,
            biHeight: -(DIB as i32),
            biPlanes: 1,
            biBitCount: 32,
            biCompression: 0,
            biSizeImage: 0,
            biXPelsPerMeter: 0,
            biYPelsPerMeter: 0,
            biClrUsed: 0,
            biClrImportant: 0,
        };
        unsafe {
            let dc = win::CreateCompatibleDC(std::ptr::null_mut());
            if dc.is_null() {
                return None;
            }
            let mut bits: *mut std::ffi::c_void = std::ptr::null_mut();
            let bitmap =
                win::CreateDIBSection(dc, &bi, win::DIB_RGB_COLORS, &mut bits, std::ptr::null_mut(), 0);
            if bitmap.is_null() || bits.is_null() {
                win::DeleteDC(dc);
                return None;
            }
            win::SelectObject(dc, bitmap);
            win::SetTextColor(dc, 0x00FF_FFFF);
            win::SetBkColor(dc, 0x0000_0000);
            win::SetTextAlign(dc, 0); // TA_LEFT | TA_TOP
            Some(Renderer {
                dc,
                bitmap,
                bits: bits as *mut u8,
            })
        }
    }

    /// 渲染一个码点到 32x32 画布, 返回绿色通道覆盖率 (0..255).
    fn coverage(&mut self, cp: u32) -> Vec<u8> {
        unsafe {
            win::PatBlt(self.dc, 0, 0, DIB as i32, DIB as i32, win::BLACKNESS);
            let text: [u16; 1] = [cp as u16];
            win::ExtTextOutW(
                self.dc,
                RENDER_X,
                RENDER_Y,
                0,
                std::ptr::null(),
                text.as_ptr(),
                1,
                std::ptr::null(),
            );
        }
        let mut cov = vec![0u8; DIB * DIB];
        for y in 0..DIB {
            for x in 0..DIB {
                let off = (y * DIB + x) * 4 + 1;
                cov[y * DIB + x] = unsafe { *self.bits.add(off) };
            }
        }
        cov
    }

    fn ascent(&self) -> i32 {
        let mut tm = [0u8; 256];
        let ok = unsafe { win::GetTextMetricsW(self.dc, tm.as_mut_ptr()) };
        if ok == 0 {
            return 0;
        }
        i32::from_le_bytes([tm[4], tm[5], tm[6], tm[7]])
    }
}

#[cfg(windows)]
impl Drop for Renderer {
    fn drop(&mut self) {
        unsafe {
            win::DeleteObject(self.bitmap);
            win::DeleteDC(self.dc);
        }
    }
}

#[cfg(windows)]
fn round_half_even(x: f64) -> i32 {
    let f = x.floor();
    let d = x - f;
    let r = if d > 0.5 {
        f + 1.0
    } else if d < 0.5 {
        f
    } else if (f as i64) % 2 == 0 {
        f
    } else {
        f + 1.0
    };
    r as i32
}

#[cfg(windows)]
fn ink_bbox(cov: &[u8]) -> Option<(usize, usize, usize, usize)> {
    let (mut l, mut t, mut r, mut b) = (DIB, DIB, usize::MAX, 0usize);
    let mut any = false;
    for y in 0..DIB {
        for x in 0..DIB {
            if cov[y * DIB + x] > 16 {
                any = true;
                if x < l {
                    l = x;
                }
                if x > r || r == usize::MAX {
                    r = x;
                }
                if y < t {
                    t = y;
                }
                if y > b {
                    b = y;
                }
            }
        }
    }
    if !any || r == usize::MAX || r < l {
        None
    } else {
        Some((l, t, r, b))
    }
}

#[cfg(windows)]
fn crop4bit(cov: &[u8], ox: i32, oy: i32) -> Vec<u8> {
    let mut cell = vec![0u8; BYTES_PER_GLYPH];
    for y in 0..CELL {
        let sy = oy + y as i32;
        if sy < 0 || sy >= DIB as i32 {
            continue;
        }
        for x in 0..CELL {
            let sx = ox + x as i32;
            if sx < 0 || sx >= DIB as i32 {
                continue;
            }
            let c = cov[sy as usize * DIB + sx as usize];
            if c < 16 {
                continue;
            }
            let lvl = ((c as u32 + 8) >> 4).min(15) as u8;
            let shift = if x & 1 == 0 { 4 } else { 0 };
            cell[y * 8 + (x >> 1)] |= lvl << shift;
        }
    }
    cell
}

#[cfg(windows)]
fn wide(cp: u32) -> bool {
    cp >= 128
}

#[cfg(windows)]
fn main() {
    let out_dir = env::args()
        .nth(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("build"));
    if let Err(e) = fs::create_dir_all(&out_dir) {
        eprintln!("[!] 无法创建输出目录: {}", e);
        exit(1);
    }

    let mut renderer = match Renderer::new() {
        Some(r) => r,
        None => {
            eprintln!("[!] CreateDIBSection 失败");
            exit(1);
        }
    };

    // 码点集合: ASCII + CJK 标点 + 汉字 + 全角
    let mut codes: Vec<u32> = Vec::with_capacity(21_500);
    codes.extend(0x20..0x7F);
    codes.extend(0x3000..0x3040);
    codes.extend(0x4E00..0xA000);
    codes.extend(0xFF00..0xFFF0);

    let candidates = ["SimHei", "Microsoft YaHei", "SimSun", "FangSong"];
    let mut selected = None;
    for name in candidates {
        let face: Vec<u16> = name.encode_utf16().chain(std::iter::once(0)).collect();
        let font = unsafe {
            win::CreateFontW(
                FONT_EM,
                0,
                0,
                0,
                400,
                0,
                0,
                0,
                win::DEFAULT_CHARSET,
                0,
                0,
                win::CLEARTYPE_QUALITY,
                win::PITCH_FAMILY,
                face.as_ptr(),
            )
        };
        if font.is_null() {
            continue;
        }
        unsafe {
            win::SelectObject(renderer.dc, font);
        }
        let cov = renderer.coverage(0x4E2D);
        if cov.iter().any(|&v| v != 0) && renderer.ascent() > 0 {
            selected = Some((name, font));
            break;
        }
        unsafe {
            win::DeleteObject(font);
        }
    }

    let (name_used, font) = match selected {
        Some(v) => v,
        None => {
            eprintln!("[!] 未找到可用中文字体");
            exit(1);
        }
    };

    let ascent = renderer.ascent();
    let baseline_cy = RENDER_Y + ascent;
    let baseline_out = ascent.min((CELL as i32) - 2);
    let oy = baseline_cy - baseline_out;
    println!("    字体: {}  ascent={}", name_used, ascent);
    println!("    基线: 画布 y={} -> 输出行 {} (原生灰度 AA)", baseline_cy, baseline_out);

    let mut font_bin: Vec<u8> = Vec::with_capacity(codes.len() * BYTES_PER_GLYPH);
    let mut blank = 0usize;
    for (i, &cp) in codes.iter().enumerate() {
        let cov = renderer.coverage(cp);
        let bbox = ink_bbox(&cov);
        match bbox {
            None => {
                font_bin.extend_from_slice(&[0u8; BYTES_PER_GLYPH]);
                blank += 1;
            }
            Some((l, _, r, _)) => {
                let cx_target = if wide(cp) { 8 } else { 4 };
                let ox = round_half_even((l + r + 1) as f64 / 2.0) - cx_target;
                let cell = crop4bit(&cov, ox, oy);
                if cell.iter().all(|&v| v == 0) {
                    blank += 1;
                }
                font_bin.extend_from_slice(&cell);
            }
        }
        if (i + 1) % 4000 == 0 {
            println!("      ... {}/{}", i + 1, codes.len());
        }
    }

    let mut cpm_bin: Vec<u8> = Vec::with_capacity(codes.len() * 2);
    for &c in &codes {
        cpm_bin.extend_from_slice(&(c as u16).to_le_bytes());
    }

    // 补齐到 256 的整数倍, 保证 font_size == count*128
    let total = (codes.len() + 255) / 256 * 256;
    let pad = total - codes.len();
    font_bin.resize(font_bin.len() + pad * BYTES_PER_GLYPH, 0);
    cpm_bin.resize(cpm_bin.len() + pad * 2, 0xFF);

    let fpath = out_dir.join("font.gtd");
    let cpath = out_dir.join("cpm.gtd");
    if let Err(e) = fs::write(&fpath, &font_bin) {
        eprintln!("[!] 写入 {} 失败: {}", fpath.display(), e);
        exit(1);
    }
    if let Err(e) = fs::write(&cpath, &cpm_bin) {
        eprintln!("[!] 写入 {} 失败: {}", cpath.display(), e);
        exit(1);
    }
    println!(
        "    字模: {} ({} 字节, {} 字, 空白 {})",
        fpath.display(),
        font_bin.len(),
        codes.len(),
        blank
    );
    println!("    码点: {} ({} 字节)", cpath.display(), cpm_bin.len());

    unsafe {
        win::DeleteObject(font);
    }
    // renderer 由 Drop 释放 DIB 与 DC
}
