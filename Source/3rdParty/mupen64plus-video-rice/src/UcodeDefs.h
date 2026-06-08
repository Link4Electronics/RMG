/*
Copyright (C) 2003 Rice1964

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#ifndef _UCODE_DEFS_H_
#define _UCODE_DEFS_H_

typedef struct {
    union {
        unsigned int w0;
        struct {
#if __BIG_ENDIAN__
            unsigned int cmd:8;
            unsigned int arg0:24;
#else
            unsigned int arg0:24;
            unsigned int cmd:8;
#endif
        };
    };
    unsigned int w1;
} Gwords;

typedef struct {
    unsigned int w0;
#if __BIG_ENDIAN__
    unsigned int flag:8;
    unsigned int v0:8;
    unsigned int v1:8;
    unsigned int v2:8;
#else
    unsigned int v2:8;
    unsigned int v1:8;
    unsigned int v0:8;
    unsigned int flag:8;
#endif
} GGBI0_Tri1;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int v2:8;
    unsigned int v1:8;
    unsigned int v0:8;
    unsigned int flag:8;
    unsigned int pad:24;
#else
    unsigned int v0:8;
    unsigned int v1:8;
    unsigned int v2:8;
    unsigned int cmd:8;
    unsigned int pad:24;
    unsigned int flag:8;
#endif
} GGBI2_Tri1;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int v5:7;
    unsigned int :1;
    unsigned int v4:7;
    unsigned int :1;
    unsigned int v3:7;
    unsigned int :1;
    unsigned int flag:8;
    unsigned int v2:7;
    unsigned int :1;
    unsigned int v1:7;
    unsigned int :1;
    unsigned int v0:7;
    unsigned int :1;
#else
    unsigned int :1;
    unsigned int v3:7;
    unsigned int :1;
    unsigned int v4:7;
    unsigned int :1;
    unsigned int v5:7;
    unsigned int cmd:8;
    unsigned int :1;
    unsigned int v0:7;
    unsigned int :1;
    unsigned int v1:7;
    unsigned int :1;
    unsigned int v2:7;
    unsigned int flag:8;
#endif
} GGBI2_Tri2;

typedef struct {
    unsigned int w0;
#if __BIG_ENDIAN__
    unsigned int v3:8;
    unsigned int v0:8;
    unsigned int v1:8;
    unsigned int v2:8;
#else
    unsigned int v2:8;
    unsigned int v1:8;
    unsigned int v0:8;
    unsigned int v3:8;
#endif
} GGBI0_Ln3DTri2;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int v3:8;
    unsigned int v4:8;
    unsigned int v5:8;

    unsigned int flag:8;
    unsigned int v0:8;
    unsigned int v1:8;
    unsigned int v2:8;
#else
    unsigned int v5:8;
    unsigned int v4:8;
    unsigned int v3:8;
    unsigned int cmd:8;

    unsigned int v2:8;
    unsigned int v1:8;
    unsigned int v0:8;
    unsigned int flag:8;
#endif
} GGBI1_Tri2;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int v5:8;
    unsigned int v4:8;
    unsigned int v3:8;

    unsigned int flag:8;
    unsigned int v2:8;
    unsigned int v1:8;
    unsigned int v0:8;
#else
    unsigned int v3:8;
    unsigned int v4:8;
    unsigned int v5:8;
    unsigned int cmd:8;

    unsigned int v0:8;
    unsigned int v1:8;
    unsigned int v2:8;
    unsigned int flag:8;
#endif
} GGBI2_Line3D;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int n:4;
    unsigned int v0:4;
    unsigned int len:16;
#else
    unsigned int len:16;
    unsigned int v0:4;
    unsigned int n:4;
    unsigned int cmd:8;
#endif
    unsigned int addr;
} GGBI0_Vtx;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int v0:7;
    unsigned int :1;
    unsigned int n:6;
    unsigned int len:10;
#else
    unsigned int len:10;
    unsigned int n:6;
    unsigned int :1;
    unsigned int v0:7;
    unsigned int cmd:8;
#endif
    unsigned int addr;
} GGBI1_Vtx;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int cmd:8;
    unsigned int :4;
    unsigned int n:8;
    unsigned int :4;
    unsigned int vend:7;
    unsigned int :1;
#else
    unsigned int :1;
    unsigned int vend:7;
    unsigned int :4;
    unsigned int n:8;
    unsigned int :4;
    unsigned int cmd:8;
#endif
    unsigned int addr;
} GGBI2_Vtx;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    fmt:3;
    unsigned int    siz:2;
    unsigned int    :7;
    unsigned int    width:12;
#else
    unsigned int    width:12;
    unsigned int    :7;
    unsigned int    siz:2;
    unsigned int    fmt:3;
    unsigned int    cmd:8;
#endif
    unsigned int    addr;
} GSetImg;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    pad:8;
    unsigned int    prim_min_level:8;
    unsigned int    prim_level:8;

    union {
        unsigned int    color;
        struct {
            unsigned int fillcolor2:16;
            unsigned int fillcolor:16;
        };
        struct {
            unsigned int r:8;
            unsigned int g:8;
            unsigned int b:8;
            unsigned int a:8;
        };
    };
#else
    unsigned int    prim_level:8;
    unsigned int    prim_min_level:8;
    unsigned int    pad:8;
    unsigned int    cmd:8;

    union {
        unsigned int    color;
        struct {
            unsigned int fillcolor:16;
            unsigned int fillcolor2:16;
        };
        struct {
            unsigned int a:8;
            unsigned int b:8;
            unsigned int g:8;
            unsigned int r:8;
        };
    };
#endif
} GSetColor;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    param:8;
    unsigned int    :16;
#else
    unsigned int    :16;
    unsigned int    param:8;
    unsigned int    cmd:8;
#endif
    unsigned int    addr;
} GGBI0_Dlist;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    :5;
    unsigned int    push:1;
    unsigned int    load:1;
    unsigned int    projection:1;
    unsigned int    len:16;
#else
    unsigned int    len:16;
    unsigned int    projection:1;
    unsigned int    load:1;
    unsigned int    push:1;
    unsigned int    :5;
    unsigned int    cmd:8;
#endif
    unsigned int    addr;
} GGBI0_Matrix;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    :24;
    unsigned int    :31;
    unsigned int    projection:1;
#else
    unsigned int    :24;
    unsigned int    cmd:8;
    unsigned int    projection:1;
    unsigned int    :31;
#endif
} GGBI0_PopMatrix;

typedef struct {
    union {
        struct {
#if __BIG_ENDIAN__
            unsigned int    cmd:8;
            unsigned int    len:16;
            unsigned int    param:8;
#else
            unsigned int    param:8;
            unsigned int    len:16;
            unsigned int    cmd:8;
#endif
        };
        struct {
#if __BIG_ENDIAN__
            unsigned int    cmd2:8;
            unsigned int    len2:16;
            unsigned int    :5;
            unsigned int    projection:1;
            unsigned int    load:1;
            unsigned int    nopush:1;
#else
            unsigned int    nopush:1;
            unsigned int    load:1;
            unsigned int    projection:1;
            unsigned int    :5;
            unsigned int    len2:16;
            unsigned int    cmd2:8;
#endif
        };
    };
    unsigned int    addr;
} GGBI2_Matrix;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    offset:16;
    unsigned int    type:8;
#else
    unsigned int    type:8;
    unsigned int    offset:16;
    unsigned int    cmd:8;
#endif
    unsigned int    value;
} GGBI0_MoveWord;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    type:8;
    unsigned int    offset:16;
#else
    unsigned int    offset:16;
    unsigned int    type:8;
    unsigned int    cmd:8;
#endif
    unsigned int    value;
} GGBI2_MoveWord;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    :10;
    unsigned int    level:3;
    unsigned int    tile:3;
    unsigned int    :6;
    unsigned int    enable_gbi2:1;
    unsigned int    enable_gbi0:1;
    unsigned int    scaleS:16;
    unsigned int    scaleT:16;
#else
    unsigned int    enable_gbi0:1;
    unsigned int    enable_gbi2:1;
    unsigned int    :6;
    unsigned int    tile:3;
    unsigned int    level:3;
    unsigned int    :10;
    unsigned int    cmd:8;
    unsigned int    scaleT:16;
    unsigned int    scaleS:16;
#endif
} GTexture;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    sl:12;
    unsigned int    tl:12;

    unsigned int    pad:5;
    unsigned int    tile:3;
    unsigned int    sh:12;
    unsigned int    th:12;
#else
    unsigned int    tl:12;
    unsigned int    sl:12;
    unsigned int    cmd:8;

    unsigned int    th:12;
    unsigned int    sh:12;
    unsigned int    tile:3;
    unsigned int    pad:5;
#endif
} Gloadtile;

typedef struct {
#if __BIG_ENDIAN__
    unsigned int    cmd:8;
    unsigned int    fmt:3;
    unsigned int    siz:2;
    unsigned int    pad0:1;
    unsigned int    line:9;
    unsigned int    tmem:9;

    unsigned int    pad1:5;
    unsigned int    tile:3;
    unsigned int    palette:4;
    unsigned int    ct:1;
    unsigned int    mt:1;
    unsigned int    maskt:4;
    unsigned int    shiftt:4;
    unsigned int    cs:1;
    unsigned int    ms:1;
    unsigned int    masks:4;
    unsigned int    shifts:4;
#else
    unsigned int    tmem:9;
    unsigned int    line:9;
    unsigned int    pad0:1;
    unsigned int    siz:2;
    unsigned int    fmt:3;
    unsigned int    cmd:8;

    unsigned int    shifts:4;
    unsigned int    masks:4;
    unsigned int    ms:1;
    unsigned int    cs:1;
    unsigned int    shiftt:4;
    unsigned int    maskt:4;
    unsigned int    mt:1;
    unsigned int    ct:1;
    unsigned int    palette:4;
    unsigned int    tile:3;
    unsigned int    pad1:5;
#endif
} Gsettile;

typedef union {
    Gwords          words;
    GGBI0_Tri1      tri1;
    GGBI0_Ln3DTri2  ln3dtri2;
    GGBI1_Tri2      gbi1tri2;
    GGBI2_Tri1      gbi2tri1;
    GGBI2_Tri2      gbi2tri2;
    GGBI2_Line3D    gbi2line3d;
    GGBI0_Vtx       gbi0vtx;
    GGBI1_Vtx       gbi1vtx;
    GGBI2_Vtx       gbi2vtx;
    GSetImg         setimg;
    GSetColor       setcolor;
    GGBI0_Dlist     gbi0dlist;
    GGBI0_Matrix    gbi0matrix;
    GGBI0_PopMatrix gbi0popmatrix;
    GGBI2_Matrix    gbi2matrix;
    GGBI0_MoveWord  gbi0moveword;
    GGBI2_MoveWord  gbi2moveword;
    GTexture        texture;
    Gloadtile       loadtile;
    Gsettile        settile;
    long long int   force_structure_alignment;
} Gfx;

typedef union {
    struct {
        unsigned int    w0;
        unsigned int    w1;
        unsigned int    w2;
        unsigned int    w3;
    };
    struct {
#if __BIG_ENDIAN__
        unsigned int    cmd:8;
        unsigned int    xl:12;
        unsigned int    yl:12;

        unsigned int    pad1:5;
        unsigned int    tile:3;
        unsigned int    xh:12;
        unsigned int    yh:12;

        unsigned int    s:16;
        unsigned int    t:16;

        unsigned int    dsdx:16;
        unsigned int    dtdy:16;
#else
        unsigned int    yl:12;
        unsigned int    xl:12;
        unsigned int    cmd:8;

        unsigned int    yh:12;
        unsigned int    xh:12;
        unsigned int    tile:3;
        unsigned int    pad1:5;

        unsigned int    t:16;
        unsigned int    s:16;

        unsigned int    dtdy:16;
        unsigned int    dsdx:16;
#endif
    };
} Gtexrect;

#endif
