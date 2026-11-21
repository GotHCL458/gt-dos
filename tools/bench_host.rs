//! TC 性能基准的宿主原生对照 (rustc -O). 输出 HOST 行, 与 GT-DOS 内 TC
//! 生成代码的结果一一对应. 使用 black_box 防止优化器消除循环.

use std::hint::black_box;
use std::time::Instant;

fn add1(x: i64) -> i64 {
    x + 1
}

fn fib(n: i64) -> i64 {
    if n < 2 {
        n
    } else {
        fib(n - 1) + fib(n - 2)
    }
}

fn main() {
    let mut i: i64;
    let mut s: i64;

    let t = Instant::now();
    s = 0;
    i = 0;
    while i < 5_000_000 {
        s = s.wrapping_add(black_box(i));
        i += 1;
    }
    println!("HOST int_loop us={}", t.elapsed().as_micros());
    black_box(s);

    let t = Instant::now();
    s = 0;
    i = 0;
    while i < 5_000_000 {
        s = s.wrapping_add(black_box(2 * 3 + 4 * 5));
        i += 1;
    }
    println!("HOST const_expr us={}", t.elapsed().as_micros());
    black_box(s);

    let t = Instant::now();
    s = 0;
    i = 0;
    while i < 1_000_000 {
        s = s.wrapping_add(black_box(add1(black_box(i))));
        i += 1;
    }
    println!("HOST direct_call us={}", t.elapsed().as_micros());
    black_box(s);

    let fp: fn(i64) -> i64 = add1;
    let t = Instant::now();
    s = 0;
    i = 0;
    while i < 1_000_000 {
        s = s.wrapping_add(black_box(fp(black_box(i))));
        i += 1;
    }
    println!("HOST indirect_call us={}", t.elapsed().as_micros());
    black_box(s);

    let t = Instant::now();
    let mut d: f64 = 0.0;
    i = 0;
    while i < 2_000_000 {
        d = d * 1.000001 + 0.5;
        i += 1;
    }
    println!("HOST double_ops us={}", t.elapsed().as_micros());
    black_box(d);

    let mut a = [0i64; 64];
    for k in 0..64 {
        a[k] = k as i64;
    }
    let t = Instant::now();
    s = 0;
    i = 0;
    while i < 5_000_000 {
        s = s.wrapping_add(black_box(a[(i & 63) as usize]));
        i += 1;
    }
    println!("HOST array_sum us={}", t.elapsed().as_micros());
    black_box(s);

    let t = Instant::now();
    let f = fib(black_box(30));
    println!("HOST fib30 us={} chk={}", t.elapsed().as_micros(), f);
    black_box(f);
}
