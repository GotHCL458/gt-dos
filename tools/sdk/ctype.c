/* GT-DOS TC 标准库: ctype.c (仅在 #include <ctype.h> 后编入程序) */

int isdigit(int c) { return c >= '0' && c <= '9'; }

int isalpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

int isalnum(int c) { return isdigit(c) || isalpha(c); }

int isspace(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v';
}

int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }

int toupper(int c)
{
    if (c >= 'a' && c <= 'z')
        return c - 32;
    return c;
}

int tolower(int c)
{
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    return c;
}
