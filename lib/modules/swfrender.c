/* swfrender.c

   functions for rendering swf content
      
   Extension module for the rfxswf library.
   Part of the swftools package.

   Copyright (c) 2004 Mederra Oy <http://www.mederra.fi>
   Copyright (c) 2004 Matthias Kramm
 
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <assert.h>

typedef struct _dummyshape
{
    SHAPE2*shape;
    //CXFORM //TODO
    struct _dummyshape*next;
} dummyshape_t;

/* one bit flag: */
#define clip_type 0
#define fill_type 1

typedef struct _renderpoint
{
    char type;
    float x;
    U32 depth;

    SHAPELINE*shapeline;
    
    U32 clipdepth;
    dummyshape_t*s;
    
} renderpoint_t;

/* 
    enum {clip_type, solidfill_type, texturefill_type, gradientfill_type} type;
    float fx;
    int x;
    U32 depth;
    U32 clipdepth;

    // solidfill;
    RGBA color; 
    
    // texturefill
    bitmap_t* bitmap;

    // gradientfill
    gradient_t* gradient;

    // texture- & gradientfill;
    U32 x,y;
    U32 dx,dy;

*/

typedef struct _renderline
{
    TAG*points; //incremented in 128 byte steps
} renderline_t;

typedef struct _bitmap {
    int width;
    int height;
    RGBA*data;
    int id;
    struct _bitmap*next;
} bitmap_t;

typedef struct _renderbuf_internal
{
    renderline_t*lines;
    bitmap_t*bitmaps;
    char antialize;
    int multiply;
    int width2,height2;
    dummyshape_t*dshapes;
    dummyshape_t*dshapes_next;
    RGBA*background;
    int background_width, background_height;
} renderbuf_internal;

#define DEBUG 0

static void renderpoint_write(TAG*tag, renderpoint_t*p)
{
    if(tag->len == 0) {
	swf_SetU32(tag, 1);
    } else {
	int num = GET32(tag->data);
	PUT32(tag->data, num+1);
    }

    swf_SetBits(tag, p->type, 1);
    swf_SetBits(tag, *(U32*)&p->x, 32);
    if(p->depth & 0xffff) {
	swf_SetBits(tag, 1, 1);
	swf_SetBits(tag, p->depth, 32);
    } else {
	swf_SetBits(tag, 0, 1);
	swf_SetBits(tag, p->depth >> 16, 16);
    }
    swf_SetBits(tag, *(U32*)&p->shapeline, 32);
    if(p->type == clip_type) {
	if(p->clipdepth & 0xffff) {
	    swf_SetBits(tag, 1, 1);
	    swf_SetBits(tag, p->clipdepth, 32);
	} else {
	    swf_SetBits(tag, 0, 1);
	    swf_SetBits(tag, p->clipdepth >> 16, 16);
	}
	/* don't set s */
    } else {
	swf_SetBits(tag, *(U32*)&p->s, 32);
	/* don't set clipdepth */
    }
}
static renderpoint_t renderpoint_read(TAG*tag, int num)
{
    renderpoint_t p;
    U8 flag = 0;
    U32 dummy = 0;

    p.type = swf_GetBits(tag, 1);
    
    dummy = swf_GetBits(tag, 32);p.x = *(float*)&dummy;
    flag = swf_GetBits(tag, 1);
    if(flag) {
	p.depth = swf_GetBits(tag, 32);
    } else {
	p.depth = swf_GetBits(tag, 16) << 16;
    }
    dummy = swf_GetBits(tag, 32);p.shapeline = *(SHAPELINE**)&dummy;
    if(p.type == clip_type) {
	flag = swf_GetBits(tag, 1);
	if(flag) {
	    p.clipdepth = swf_GetBits(tag, 32);
	} else {
	    p.clipdepth = swf_GetBits(tag, 16) << 16;
	}
	p.s = 0;
    } else {
	dummy = swf_GetBits(tag, 32);p.s = *(dummyshape_t**)&dummy;
	p.clipdepth = 0;
    }

    return p;
}

static int renderpoint_num(TAG*tag)
{
    if(tag->len == 0)
	return 0;
    return GET32(tag->data);
}

static renderpoint_t* renderpoint_readall(TAG*tag)
{
    int num;
    int t;
    renderpoint_t*p;
    swf_SetTagPos(tag, 0);
    if(tag->len == 0)
	num = 0;
    else
	num = swf_GetU32(tag);
    p = (renderpoint_t*)rfx_alloc(num*sizeof(renderpoint_t));
    for(t=0;t<num;t++)
	p[t] = renderpoint_read(tag,t);
    return p;
}

static inline void add_pixel(RENDERBUF*dest, float x, int y, renderpoint_t*p)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    if(x >= i->width2 || y >= i->height2 || y<0) return;
    p->x = x;
    if(y<10)
	renderpoint_write(i->lines[y].points, p);
    else
	renderpoint_write(i->lines[y].points, p);
}

/* set this to 0.777777 or something if the "both fillstyles set while not inside shape"
   problem appears to often */
#define CUT 0.5

static void add_line(RENDERBUF*buf, double x1, double y1, double x2, double y2, renderpoint_t*p)
{
    renderbuf_internal*i = (renderbuf_internal*)buf->internal;
    double diffx, diffy;
    double ny1, ny2, stepx;
/*    if(DEBUG&4) {
        int l = sqrt((x2-x1)*(x2-x1) + (y2-y1)*(y2-y1));
        printf(" l[%d - %.2f/%.2f -> %.2f/%.2f]", l, x1/20.0, y1/20.0, x2/20.0, y2/20.0);
    }*/

    y1=y1*i->multiply;
    y2=y2*i->multiply;
    x1=x1*i->multiply;
    x2=x2*i->multiply;
    
    y1 = y1/20.0;
    y2 = y2/20.0;
    x1 = x1/20.0;
    x2 = x2/20.0;

    if(y2 < y1) {
        double x;
        double y;
	x = x1;x1 = x2;x2=x;
	y = y1;y1 = y2;y2=y;
    }
    
    diffx = x2 - x1;
    diffy = y2 - y1;
    
    ny1 = (int)(y1)+CUT;
    ny2 = (int)(y2)+CUT;

    if(ny1 < y1) {
        ny1 = (int)(y1) + 1.0 + CUT;
    }
    if(ny2 >= y2) {
        ny2 = (int)(y2) - 1.0 + CUT;
    }

    if(ny1 > ny2)
        return;

    stepx = diffx/diffy;
    x1 = x1 + (ny1-y1)*stepx;
    x2 = x2 + (ny2-y2)*stepx;

    {
	int posy=(int)ny1;
	int endy=(int)ny2;
	double posx=0;
	double startx = x1;

	while(posy<=endy) {
	    float xx = (float)(startx + posx);
	    add_pixel(buf, xx ,posy, p);
	    posx+=stepx;
	    posy++;
	}
    }
}
#define PI 3.14159265358979
static void add_solidline(RENDERBUF*buf, double x1, double y1, double x2, double y2, int width, renderpoint_t*p)
{
    renderbuf_internal*i = (renderbuf_internal*)buf->internal;

    double dx = x2-x1;
    double dy = y2-y1;
    double sd;
    double d;

    int t;
    int segments;
    double lastx,lasty;
    double vx,vy;
    double xx,yy;
  
    /* Make sure the line is always at least one pixel wide */
#ifdef LINEMODE1
    /* That's what Macromedia's Player does at least at zoom level >= 1.  */
    width += 20;
#else
    /* That's what Macromedia's Player seems to do at zoom level 0.  */
    /* TODO: needs testing */
    if(width<20)
	width = 20;
#endif

    sd = (double)dx*(double)dx+(double)dy*(double)dy;
    d = sqrt(sd);

    if(!dx && !dy) {
        vx = 1;
        vy = 0;
    } else {
        vx = ( dy/d);
        vy = (-dx/d);
    }

    segments = width/2;
    if(segments < 2)
        segments = 2;

    segments = 8;

    vx=vx*width*0.5;
    vy=vy*width*0.5;

    xx = x2+vx;
    yy = y2+vy;
    add_line(buf, x1+vx, y1+vy, xx, yy, p);
    lastx = xx;
    lasty = yy;
    for(t=1;t<segments;t++) {
        double s = sin(t*PI/segments);
        double c = cos(t*PI/segments);
        xx = (x2 + vx*c - vy*s);
        yy = (y2 + vx*s + vy*c);
        add_line(buf, lastx, lasty, xx, yy, p);
        lastx = xx;
        lasty = yy;
    }
    
    xx = (x2-vx);
    yy = (y2-vy);
    add_line(buf, lastx, lasty, xx, yy, p);
    lastx = xx;
    lasty = yy;
    xx = (x1-vx);
    yy = (y1-vy);
    add_line(buf, lastx, lasty, xx, yy, p);
    lastx = xx;
    lasty = yy;
    for(t=1;t<segments;t++) {
        double s = sin(t*PI/segments);
        double c = cos(t*PI/segments);
        xx = (x1 - vx*c + vy*s);
        yy = (y1 - vx*s - vy*c);
        add_line(buf, lastx, lasty, xx, yy, p);
        lastx = xx;
        lasty = yy;
    }
    add_line(buf, lastx, lasty, (x1+vx), (y1+vy), p);
}

static inline void transform_point(MATRIX*m, int x, int y, int*dx, int*dy)
{
    SPOINT p,d;
    p.x = x;
    p.y = y;
    d = swf_TurnPoint(p, m);
    *dx = d.x;
    *dy = d.y;
}

static int compare_renderpoints(const void * _a, const void * _b)
{
    renderpoint_t*a = (renderpoint_t*)_a;
    renderpoint_t*b = (renderpoint_t*)_b;
    if(a->x < b->x) return -1;
    if(a->x > b->x) return 1;
    return 0;
}

void swf_Render_Init(RENDERBUF*buf, int posx, int posy, int width, int height, char antialize, int multiply)
{
    renderbuf_internal*i;
    int y;
    memset(buf, 0, sizeof(RENDERBUF));
    buf->width = width*multiply;
    buf->height = height*multiply;
    buf->posx = posx;
    buf->posy = posy;
    buf->internal = (renderbuf_internal*)rfx_calloc(sizeof(renderbuf_internal));
    i = (renderbuf_internal*)buf->internal;
    i->antialize = !!antialize;
    i->multiply = antialize?multiply*2:multiply;
    i->height2 = antialize?2*buf->height:buf->height;
    i->width2 = antialize?2*buf->width:buf->width;
    i->lines = (renderline_t*)rfx_alloc(i->height2*sizeof(renderline_t));
    for(y=0;y<i->height2;y++) {
        i->lines[y].points = swf_InsertTag(0, 0);
    }
}
void swf_Render_SetBackground(RENDERBUF*buf, RGBA*img, int width, int height)
{
    renderbuf_internal*i = (renderbuf_internal*)buf->internal;
    RGBA*bck = (RGBA*)rfx_alloc(sizeof(RGBA)*width*height);
    memcpy(bck, img, sizeof(RGBA)*width*height);
    i->background = bck;
    i->background_width = width;
    i->background_height = height;
}
void swf_Render_SetBackgroundColor(RENDERBUF*buf, RGBA color)
{
    swf_Render_SetBackground(buf, &color, 1, 1);
}
void swf_Render_AddImage(RENDERBUF*buf, U16 id, RGBA*img, int width, int height)
{
    renderbuf_internal*i = (renderbuf_internal*)buf->internal;

    bitmap_t*bm = rfx_calloc(sizeof(bitmap_t));
    bm->id = id;
    bm->width = width;
    bm->height = height;
    bm->data = img;

    bm->next = i->bitmaps;
    i->bitmaps = bm;
}
void swf_Render_ClearCanvas(RENDERBUF*dest)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    int y;
    for(y=0;y<i->height2;y++) {
        swf_ClearTag(i->lines[y].points);
    }
}
void swf_Render_Delete(RENDERBUF*dest)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    int y;
    bitmap_t*b = i->bitmaps;
    dummyshape_t*d = i->dshapes;

    if(i->background) {
	free(i->background);i->background=0;
    }

    /* delete line buffers */
    for(y=0;y<i->height2;y++) {
        swf_DeleteTag(i->lines[y].points);
        i->lines[y].points = 0;
    }

    while(d) {
        dummyshape_t*next = d->next;
        swf_Shape2Free(d->shape);
        free(d->shape);d->shape=0;
        free(d);
        d=next;
    }
    i->dshapes = 0;
    
    /* delete bitmaps */
    while(b) {
        bitmap_t*next = b->next;
        //free(b->data);b->data=0;
        rfx_free(b);
        b = next;
    }

    rfx_free(i->lines); i->lines = 0;
    rfx_free(dest->internal); dest->internal = 0;
}

static void swf_Render_AddShape(RENDERBUF*dest,dummyshape_t*s)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;

    s->next = 0;
    if(i->dshapes_next)
        i->dshapes_next->next = s;
    i->dshapes_next = s;
    if(!i->dshapes) {
        i->dshapes = s;
    }
}

static SHAPE2* linestyle2fillstyle(SHAPE2*shape)
{
    SHAPE2*s = rfx_calloc(sizeof(SHAPE2));
    int t;
    s->numfillstyles = shape->numlinestyles;
    s->fillstyles = (FILLSTYLE*)rfx_calloc(sizeof(FILLSTYLE)*shape->numlinestyles);
    s->lines = (SHAPELINE*)rfx_calloc(sizeof(SHAPELINE)*shape->numlinestyles);
    for(t=0;t<shape->numlinestyles;t++) {
        s->lines[t].fillstyle0 = t+1;
        s->fillstyles[t].type = FILL_SOLID;
        s->fillstyles[t].color = shape->linestyles[t].color;
    }
    return s;
}

void swf_RenderShape(RENDERBUF*dest, SHAPE2*shape, MATRIX*m, CXFORM*c, U16 _depth,U16 _clipdepth)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    
    SHAPELINE*line = shape->lines;
    int x=0,y=0;
    MATRIX mat = *m;
    SHAPE2* lshape = 0;

    renderpoint_t p, lp;
    memset(&p, 0, sizeof(renderpoint_t));
    memset(&lp, 0, sizeof(renderpoint_t));
    
    p.type = _clipdepth?clip_type:fill_type;
    p.depth = _depth << 16;
    p.clipdepth = _clipdepth << 16;

    mat.tx -= dest->posx*20;
    mat.ty -= dest->posy*20;

    if(shape->numfillstyles) {
        dummyshape_t*fshape = rfx_calloc(sizeof(dummyshape_t));
        int t;
        SHAPE2* s2 = swf_Shape2Clone(shape);
       
        fshape->shape = s2;

        p.s = fshape;

        /* multiply fillstyles matrices with placement matrix-
           important for texture and gradient fill */
        for(t=0;t<s2->numfillstyles;t++) {
            MATRIX nm;
            swf_MatrixJoin(&nm, &s2->fillstyles[t].m, &mat); //TODO: is this the right order?
            nm.sx *= i->multiply;
            nm.sy *= i->multiply;
            nm.r0 *= i->multiply;
            nm.r1 *= i->multiply;
            nm.tx *= i->multiply;
            nm.ty *= i->multiply;
            s2->fillstyles[t].m = nm;
        }

        /* add this shape to the global shape list, for deallocing */
        swf_Render_AddShape(dest, fshape);
    }

    if(shape->numlinestyles) {
        dummyshape_t*dshape = rfx_calloc(sizeof(dummyshape_t));
        
        lshape = linestyle2fillstyle(shape);
            
        lp.type = fill_type;
        lp.s = dshape;
        lp.depth = (_depth << 16)+1;

        dshape->shape = lshape;

        /* add this shape to the global shape list, for deallocing */
        swf_Render_AddShape(dest, dshape);
    }

    if(p.clipdepth) {
        /* reverse shape */
        p.shapeline = 0;
        add_line(dest, -20, 0, -20, i->height2*20, &p);
    }

    while(line)
    {
        int x1,y1,x2,y2,x3,y3;

        p.shapeline = line;

        if(line->type == moveTo) {
        } else if(line->type == lineTo) {
            if(DEBUG&4) {
		int l;
                x1 = x;
                y1 = y;
                x2 = line->x;
                y2 = line->y;
                l = sqrt((x2-x1)*(x2-x1) + (y2-y1)*(y2-y1));
                printf("%d - %.2f/%.2f -> %.2f/%.2f ", l, x1/20.0, y1/20.0, x2/20.0, y2/20.0);
            }

            transform_point(&mat, x, y, &x1, &y1);
            transform_point(&mat, line->x, line->y, &x3, &y3);
            
            if(line->linestyle && ! p.clipdepth) {
                lp.shapeline = &lshape->lines[line->linestyle-1];
                add_solidline(dest, x1, y1, x3, y3, shape->linestyles[line->linestyle-1].width, &lp);
                lp.depth++;
            }
            if(line->fillstyle0 || line->fillstyle1) {
                assert(shape->numfillstyles);
                add_line(dest, x1, y1, x3, y3, &p);
            }
            
            if(DEBUG&4) printf("\n");
        } else if(line->type == splineTo) {
	    int c,t,parts,qparts;
	    double xx,yy;
            
            transform_point(&mat, x, y, &x1, &y1);
            transform_point(&mat, line->sx, line->sy, &x2, &y2);
            transform_point(&mat, line->x, line->y, &x3, &y3);
            
            c = abs(x3-2*x2+x1) + abs(y3-2*y2+y1);
            xx=x1;
	    yy=y1;

            parts = (int)(sqrt(c)/3);
            if(!parts) parts = 1;

            if(DEBUG&4)
            {
                printf("spline %.2f/%.2f -(%.2f/%.2f)-> %.2f/%.2f (c=%d, %d parts)", 
                        x1/20.0, y1/20.0, 
                        x2/20.0, y2/20.0, 
                        x3/20.0, y3/20.0, c, parts);
            }

            for(t=1;t<=parts;t++) {
                double nx = (double)(t*t*x3 + 2*t*(parts-t)*x2 + (parts-t)*(parts-t)*x1)/(double)(parts*parts);
                double ny = (double)(t*t*y3 + 2*t*(parts-t)*y2 + (parts-t)*(parts-t)*y1)/(double)(parts*parts);
                
                if(line->linestyle && ! p.clipdepth) {
                    lp.shapeline = &lshape->lines[line->linestyle-1];
                    add_solidline(dest, xx, yy, nx, ny, shape->linestyles[line->linestyle-1].width, &lp);
                    lp.depth++;
                }
                if(line->fillstyle0 || line->fillstyle1) {
                    assert(shape->numfillstyles);
                    add_line(dest, (int)xx, (int)yy, (int)nx, (int)ny, &p);
                }

                xx = nx;
                yy = ny;
            }
            if(DEBUG&4) 
                printf("\n");
        }
        x = line->x;
        y = line->y;
        line = line->next;
    }
}

typedef struct _layer {
    int fillid;
    U32 clipdepth;
    renderpoint_t*p;
    struct _layer*next;
    struct _layer*prev;
} layer_t;

typedef struct {
    layer_t*layers;
} state_t;

static RGBA color_red = {255,255,0,0};
static RGBA color_white = {255,255,255,255};

static void fill_plain(RGBA*line, int x1, int x2, RGBA col)
{
    int x = x1;
    if(col.a!=255) {
        int ainv = 255-col.a;
        col.r = (col.r*col.a)>>8;
        col.g = (col.g*col.a)>>8;
        col.b = (col.b*col.a)>>8;
        col.a = 255;
        do {
            line[x].r = ((line[x].r*ainv)>>8)+col.r;
            line[x].g = ((line[x].g*ainv)>>8)+col.g;
            line[x].b = ((line[x].b*ainv)>>8)+col.b;
            line[x].a = 255;
        } while(++x<x2);
    } else {
        do {
            line[x] = col;
        } while(++x<x2);
    }
}

static void fill_bitmap(RGBA*line, int y, int x1, int x2, MATRIX*m, bitmap_t*b, int clip)
{
    int x = x1;
    double m11=m->sx/65536.0, m21=m->r1/65536.0;
    double m12=m->r0/65536.0, m22=m->sy/65536.0;
    double rx = m->tx/20.0;
    double ry = m->ty/20.0;
    double det = m11*m22 - m12*m21;
    if(fabs(det) < 0.0005) { 
	/* x direction equals y direction- the image is invisible */
	return;
    }
    det = 20.0/det;
 
    if(!b->width || !b->height) {
        fill_plain(line, x1, x2, color_red);
        return;
    }

    do {
	RGBA col;
        int xx = (int)((  (x - rx) * m22 - (y - ry) * m21)*det);
        int yy = (int)((- (x - rx) * m12 + (y - ry) * m11)*det);
	int ainv;
        
        if(clip) {
            if(xx<0) xx=0;
            if(xx>=b->width) xx = b->width-1;
            if(yy<0) yy=0;
            if(yy>=b->height) yy = b->height-1;
        } else {
            xx %= b->width;
            yy %= b->height;
        }

        col = b->data[yy*b->width+xx];
        ainv = 255-col.a;

        line[x].r = ((line[x].r*ainv)>>8)+col.r;
        line[x].g = ((line[x].g*ainv)>>8)+col.g;
        line[x].b = ((line[x].b*ainv)>>8)+col.b;
        line[x].a = 255;
    } while(++x<x2);
}

static void fill(RENDERBUF*dest, RGBA*line, int y, int x1, int x2, state_t*clipstate, state_t*fillstate)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    U32 clipdepth;

    layer_t*lc = clipstate->layers;
    layer_t*lf = fillstate->layers;
    layer_t*l = 0;

    if(x1>=x2) //zero width? nothing to do.
        return;
    
    clipdepth = 0;
    while(lf) {
	if(lc && (!lf || lc->p->depth < lf->p->depth)) {
	    l = lc;
	    lc = lc->next;
	} else if(lf && (!lc || lf->p->depth < lc->p->depth)) {
	    l = lf;
	    lf = lf->next;
	} else if(lf && lc && lf->p->depth == lc->p->depth) {
	    /* A clipshape and a fillshape at the same depth. Yuck.
	       Bug in the SWF file */
	    fprintf(stderr, "Error: Multiple use of depth %d in SWF\n", lf->p->depth);
	    l = lc;
	    lc = lc->next;
	} else {
	    fprintf(stderr, "Internal error: %08x %08x\n", lc, lf);
	    if(lc) fprintf(stderr, "                lc->depth = %08x\n", lc->p->depth);
	    if(lf) fprintf(stderr, "                lf->depth = %08x\n", lf->p->depth);
	}

        if(l->p->depth < clipdepth) {
            if(DEBUG&2) printf("(clipped)");
	    continue;
        }
        if(l->fillid < 0 /*clip*/) {
            if(DEBUG&2) printf("(add clip %d)", l->clipdepth);
            if(l->clipdepth > clipdepth)
                clipdepth = l->clipdepth;
        } else if(l->fillid == 0) {
            /* not filled. TODO: we should never add those in the first place */
            if(DEBUG&2)
                printf("(not filled)");
        } else if(l->fillid > l->p->s->shape->numfillstyles) {
            fprintf(stderr, "Fill style out of bounds (%d>%d)", l->fillid, l->p->s->shape->numlinestyles);
        } else {
            FILLSTYLE*f;
            if(DEBUG&2) 
                printf("(%d -> %d style %d)", x1, x2, l->fillid);

            f = &l->p->s->shape->fillstyles[l->fillid-1];

            if(f->type == FILL_SOLID) {
                /* plain color fill */
                fill_plain(line, x1, x2, f->color);
            } else if(f->type == FILL_TILED || f->type == FILL_CLIPPED) {
                /* TODO: optimize (do this in add_pixel()?) */
                bitmap_t* b = i->bitmaps;
                while(b && b->id != f->id_bitmap) {
                    b = b->next;
                }
                if(!b) {
                    fprintf(stderr, "Shape references unknown bitmap %d\n", f->id_bitmap);
                    fill_plain(line, x1, x2, color_red);
                } else {
                    //done in swf_RenderShape now
                    //MATRIX m = f->m;
                    //m.tx -= dest->posx*20;
                    //m.ty -= dest->posy*20;
                    //m.sx *= i->multiply;
                    //m.sy *= i->multiply;
                    //m.r0 *= i->multiply;
                    //m.r1 *= i->multiply;
                    //m.tx *= i->multiply;
                    //m.ty *= i->multiply;
                    fill_bitmap(line, y, x1, x2, &f->m, b, FILL_CLIPPED?1:0);
                }
            }
        }
    }
}

static void search_layer(state_t*state, int depth, layer_t**before, layer_t**self, layer_t**after)
{
    layer_t*last=0,*l = state->layers;
    while(l && l->p->depth < depth) {
        last = l;
        l = l->next;
    }
    *before = last;
    if(l && l->p->depth == depth)
        *self = l;
    else
        *after = l;
}
static void delete_layer(state_t*state, layer_t*todel)
{
    layer_t*before=todel->prev;
    layer_t*next = todel->next;
    rfx_free(todel);
    if(!before) {
        state->layers = next;
        if(next)
            next->prev = 0;
    } else {
        before->next = next;
        if(before->next)
            before->next->prev = before;
    }
}
static void add_layer(state_t*state, layer_t*before, layer_t*toadd)
{
    if(!before) {
        toadd->next = state->layers;
        toadd->prev = 0;
        state->layers=toadd;
    } else {
        toadd->next = before->next;
        toadd->prev = before;
        before->next = toadd;
    }
    if(toadd->next)
        toadd->next->prev = toadd;
}
static void free_layers(state_t* state)
{
    layer_t*l = state->layers;
    while(l) {
        layer_t*next = l->next;
        rfx_free(l);
        l = next;
    }
}

static void change_state(int y, state_t* state, renderpoint_t*p)
{
    layer_t*before=0, *self=0, *after=0;

    if(DEBUG&2) { 
        printf("[%s(%d,%d)/%d/%d-%d]", p->type==clip_type?"C":"F", p->x, y, p->depth, p->shapeline->fillstyle0, p->shapeline->fillstyle1);
    }

    search_layer(state, p->depth, &before, &self, &after);

    if(self) {
        /* shape update */
        if(self->fillid<0 || !p->shapeline->fillstyle0 || !p->shapeline->fillstyle1) {
            /* filling/clipping ends */
            if(DEBUG&2) printf("<D>");
            
            delete_layer(state, self);
        } else { 
            /*both fill0 and fill1 are set- exchange the two, updating the layer */
            if(self->fillid == p->shapeline->fillstyle0) {
                self->fillid = p->shapeline->fillstyle1;
                self->clipdepth = 0;
                self->p = p;
                if(DEBUG&2) printf("<X>");
            } else if(self->fillid == p->shapeline->fillstyle1) {
                self->fillid = p->shapeline->fillstyle0;
                self->clipdepth = 0;
                self->p = p;
                if(DEBUG&2) printf("<X>");
            } else {
                /* buggy shape. keep everything as-is. */
                if(DEBUG&2) printf("<!>");
                //fprintf(stderr, "<line %d: bad swap>\n", y);
            }
        }
        return;
    } else {
        layer_t* n = 0;
        if(p->shapeline && p->shapeline->fillstyle0 && p->shapeline->fillstyle1) {
            /* this is a hack- a better way would be to make sure that
               we always get (0,32), (32, 33), (33, 0) in the right order if
               they happen to fall on the same pixel.
               (not: (0,32), (33, 0), (32, 33))
            */
            fprintf(stderr, "<line %d: both fillstyles set while not inside shape>\n", y);
            return;
        }
        
        n = rfx_calloc(sizeof(layer_t));

        if(DEBUG&2) printf("<+>");

        if(p->type == clip_type) {
            /* add clipping */
            n->fillid = -1;
            n->clipdepth = p->clipdepth;
            n->p = p;
        } else {
            n->fillid = p->shapeline->fillstyle0 ? p->shapeline->fillstyle0 : p->shapeline->fillstyle1;
            n->clipdepth = 0;
            n->p = p;
        }

        add_layer(state, before, n);
    }
}

RGBA* swf_Render(RENDERBUF*dest)
{
    renderbuf_internal*i = (renderbuf_internal*)dest->internal;
    RGBA* img = (RGBA*)rfx_alloc(sizeof(RGBA)*dest->width*dest->height);
    int y;
    long memory = 0;
    RGBA * line1 = rfx_alloc(sizeof(RGBA)*i->width2);
    RGBA * line2 = rfx_alloc(sizeof(RGBA)*i->width2);

    printf("%d\n", sizeof(renderpoint_t));

    for(y=0;y<i->height2;y++) {
        TAG*tag = i->lines[y].points;
        int n;
        int num = renderpoint_num(tag);
	renderpoint_t*points = renderpoint_readall(tag);
        RGBA*line = line1;
	state_t clipstate;
	state_t fillstate;
        memset(&clipstate, 0, sizeof(state_t));
        memset(&fillstate, 0, sizeof(state_t));

        if((y&1) && i->antialize)
            line = line2;

	if(!i->background) {
	    memset(line, 0, sizeof(RGBA)*i->width2);
	} else {
	    int x,xx;
	    int xstep=i->background_width*65536/i->width2;
	    RGBA*src = &i->background[(i->background_height*y/i->height2)*i->background_width];
	    for(x=0,xx=0;x<i->width2;x++,xx+=xstep) {
		line[x] = src[xx>>16];
	    }
	}
        memory += tag->memsize;
        qsort(points, num, sizeof(renderpoint_t), compare_renderpoints);
        for(n=0;n<num;n++) {
            renderpoint_t*p = &points[n];
            renderpoint_t*next= n<num-1?&points[n+1]:0;
            int startx = p->x;
            int endx = next?next->x:i->width2;
            if(endx > i->width2)
                endx = i->width2;
            if(startx < 0)
                startx = 0;

	    if(p->type == clip_type)
		change_state(y, &clipstate, p);
	    else
		change_state(y, &fillstate, p);

            fill(dest, line, y, startx, endx, &clipstate, &fillstate);
            if(endx == i->width2)
                break;
        }
        free_layers(&clipstate);
        free_layers(&fillstate);
        if(DEBUG&2) printf("\n");

        if(!i->antialize) {
            memcpy(&img[y*dest->width], line, sizeof(RGBA)*dest->width);
        } else {
            if(y&1) {
                int x;
                RGBA* p = &img[(y/2)*dest->width];
                for(x=0;x<dest->width;x++) {
                    RGBA*p1 = &line1[x*2];
                    RGBA*p2 = &line1[x*2+1];
                    RGBA*p3 = &line2[x*2];
                    RGBA*p4 = &line2[x*2+1];
                    p[x].r = (p1->r + p2->r + p3->r + p4->r)/4;
                    p[x].g = (p1->g + p2->g + p3->g + p4->g)/4;
                    p[x].b = (p1->b + p2->b + p3->b + p4->b)/4;
                    p[x].a = (p1->a + p2->a + p3->a + p4->a)/4;
                }
            }
        }
	free(points);
    }
    free(line1);
    free(line2);
    
    if(DEBUG) printf("\nMemory used: %d\n", memory);
#ifdef STATISTICS
    if(DEBUG) printf("Statistics:\n");
    if(DEBUG) printf("Average layer depth: %f\n", (double)layers/layernum);
#endif


    return img;
}
