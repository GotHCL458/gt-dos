/* GT-DOS TC 标准库: stdarg.h
   va_list 是 char*; va_start / va_arg / va_end 由编译器内建处理,
   用户可变参数函数最多 6 个实参 (含命名参数). */
#ifndef _GT_STDARG_H
#define _GT_STDARG_H

typedef char *va_list;

#endif
