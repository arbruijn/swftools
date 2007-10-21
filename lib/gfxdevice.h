#ifndef __gfxdevice_h__
#define __gfxdevice_h__

#ifdef __cplusplus
extern "C" {
#endif

#define GFX_SUBPIXEL 2560

typedef double gfxcoord_t;
    
typedef enum {gfx_moveTo, gfx_lineTo, gfx_splineTo} gfx_linetype;
typedef enum {gfx_joinMiter, gfx_joinRound, gfx_joinBevel} gfx_joinType;
typedef enum {gfx_capButt, gfx_capRound, gfx_capSquare} gfx_capType;

typedef struct _gfxline
{
    gfx_linetype type;
    gfxcoord_t x,y;
    gfxcoord_t sx,sy;
    struct _gfxline*next; /*NULL=end*/
} gfxline_t;

typedef struct _gfxglyph
{
    gfxline_t*line;
    gfxcoord_t advance;

    int unicode; // array?
    const char*name;
} gfxglyph_t;

typedef struct _gfxfont
{
    const char*id;
    int num_glyphs;
    int max_unicode;
    gfxglyph_t*glyphs;
    int* unicode2glyph;
} gfxfont_t;

typedef struct _gfxcolor
{
    unsigned char a;
    unsigned char r;
    unsigned char g;
    unsigned char b;
} gfxcolor_t;

typedef struct _gfxmatrix
{
    double m00,m10,tx;
    double m01,m11,ty;
} gfxmatrix_t;

typedef struct _gfximage
{
    /* if the data contains an alpha layer (a != 255), the
       r,g,b values will have to be premultiplied */
    gfxcolor_t*data;
    int width;
    int height;
} gfximage_t;

typedef enum {gfxgradient_radial, gfxgradient_linear} gfxgradienttype_t;

typedef struct _gfxgradient
{
    gfxcolor_t color;
    float pos; // 0.0 - 1.0
    struct _gfxgradient*next; //NULL=end
} gfxgradient_t;

typedef struct _gfxcxform
{
    float rr,rg,rb,ra, tr;
    float gr,gg,gb,ga, tg;
    float br,bg,bb,ba, tb;
    float ar,ag,ab,aa, ta;
} gfxcxform_t;

typedef struct _gfxbbox
{
    gfxcoord_t xmin, ymin, xmax, ymax;
} gfxbbox_t;

typedef struct _gfxresult
{
    void (*write)(struct _gfxresult*gfx, int filedesc);
    int (*save)(struct _gfxresult*gfx, const char*filename);
    void* (*get)(struct _gfxresult*gfx, const char*name);
    void (*destroy)(struct _gfxresult*gfx);

    void*internal;
} gfxresult_t;

typedef struct _gfxdevice
{
    const char* name; // gfx device name

    int (*setparameter)(struct _gfxdevice*dev, const char*key, const char*value);

    void (*startpage)(struct _gfxdevice*dev, int width, int height);

    void (*startclip)(struct _gfxdevice*dev, gfxline_t*line);
    void (*endclip)(struct _gfxdevice*dev);
    void (*stroke)(struct _gfxdevice*dev, gfxline_t*line, gfxcoord_t width, gfxcolor_t*color, gfx_capType cap_style, gfx_joinType joint_style, gfxcoord_t miterLimit);
    void (*fill)(struct _gfxdevice*dev, gfxline_t*line, gfxcolor_t*color);
    void (*fillbitmap)(struct _gfxdevice*dev, gfxline_t*line, gfximage_t*img, gfxmatrix_t*imgcoord2devcoord, gfxcxform_t*cxform); //cxform? tiling?
    void (*fillgradient)(struct _gfxdevice*dev, gfxline_t*line, gfxgradient_t*gradient, gfxgradienttype_t type, gfxmatrix_t*matrix); //?

    /* deprecated */ void (*addfont)(struct _gfxdevice*dev, gfxfont_t*font);

    void (*drawchar)(struct _gfxdevice*dev, gfxfont_t*font, int glyph, gfxcolor_t*color, gfxmatrix_t*matrix);

    void (*drawlink)(struct _gfxdevice*dev, gfxline_t*line, const char*action);
    
    void (*endpage)(struct _gfxdevice*dev);
    
    gfxresult_t* (*finish)(struct _gfxdevice*dev);

    void* internal;
} gfxdevice_t;

#ifdef __cplusplus
}
#endif

#endif //__gfxdevice_h__
