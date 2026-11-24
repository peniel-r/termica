# Third-party notices

## tomlc17

The settings parser is [tomlc17](https://github.com/cktan/tomlc17), fetched by
CMake at configure time.

```text
MIT License

Copyright (c) 2024-2026, CK Tan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## PCRE2

Scrollback regular-expression search uses [PCRE2](https://www.pcre2.org/),
fetched by CMake and linked statically.

```text
Copyright (c) 1997-2007 University of Cambridge
Copyright (c) 2007-2024 Philip Hazel
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notices,
  this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notices,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.
* Neither the name of the University of Cambridge nor the names of any
  contributors may be used to endorse or promote products derived from this
  software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

The binary redistribution condition does not apply all the way down a chain
of software: a binary package including PCRE2 must comply, but software
including that package need not do so unless it uses PCRE2 independently.
```

## Maple Mono NF

The built-in terminal font is [Maple Mono NF](https://github.com/subframe7536/maple-font)
7.9 by subframe7536, compiled into the executable as RCDATA resources (IDs 1-4).
Its four faces are licensed under the SIL Open Font License 1.1; the full license
and copyright are in `assets/fonts/LICENSE.txt`.

```text
Copyright 2022 The Maple Mono Project Authors
Licensed under the SIL Open Font License, Version 1.1.
See assets/fonts/LICENSE.txt for the full license.
```

## Symbols Nerd Font Mono

[Symbols Nerd Font Mono](https://www.nerdfonts.com/), part of the Nerd Fonts
project by Ryan L. McIntyre, is compiled into the executable as RCDATA
resource ID 5 and used as a last-resort glyph fallback for symbol codepoints
the primary font lacks - notably those above U+FFFF, which the glyph cache
does not cover. Its full license is in
`assets/fonts/SymbolsNerdFontMono-LICENSE.txt`.

```text
The MIT License (MIT)

Copyright (c) 2014 Ryan L McIntyre
```
