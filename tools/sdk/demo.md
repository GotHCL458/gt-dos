# GT-DOS Rich Text

This is **bold**, *italic*, ***bold italic***, ~~struck~~, `inline code`,
a [link](https://example.com) and an image ![logo](logo.png).

Plain <b>HTML bold</b>, <i>italic</i> and <c=red>colored</c> text.

## Lists and Quotes

- Arrays, structs and unions
- switch / case / default
  - nested item
  - [x] done task
  - [ ] open task

1. First item
2. Second item
3. Third item

> Rich text is rendered by the kernel.
>> Nested quotes work too.

Setext Heading
==============

### Code

```c
int main(void) {
    printf("hello %d\n", 42);   // comment
    return 0;
}
```

    indented code line

### Table

| Left | Center | Right |
|:-----|:------:|------:|
| a    | b      | c     |
| long | mid    | 42    |

---

The quick brown fox jumps over the lazy dog.
