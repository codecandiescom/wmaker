#include <proplist.h>
#include <WINGs.h>
#include <WINGsP.h>
#include "generic.h"

#ifdef USE_FREETYPE
#include <freetype/freetype.h>
#endif

#define MAX_GLYPHS 256

#define _debug(f...) {fprintf(stderr, "debug: ");fprintf(stderr, ##f);fflush(stderr);}

/* #define _debug(s) printf(s);*/

/* drawPlainString */
static Display *ds_dpy = 0;
static Colormap ds_cmap;
static RContext *rc = 0;

RColor black_color = {0, 0, 0, 0};

#ifdef USE_FREETYPE
FT_Library ft_library;
static int inst_ft_library = 0;

typedef struct __FreeTypeRImage{
    RImage *image;
    int advance_x;
    int advance_y;
    int left;
    int top;
} WMFreeTypeRImage;

typedef struct __FreeTypeData{
    FT_Face face;
    RColor color;
    WMFreeTypeRImage **glyphs_array;
    WMFreeTypeRImage **glyphs_shadow_array;
    void (*strwidth)();
} WMFreeTypeData;
#endif /* USE_FREETYPE */

int getColor (const char *string, Colormap cmap, XColor *xcolor) {
	if (!XParseColor (ds_dpy, cmap, string, xcolor)) {
        return 0;
	}
    if (!XAllocColor (ds_dpy, cmap, xcolor)) {
        return 0;
    }

	return 1;
}

void
initDrawPlainString(Display *dpy, Colormap *cmap) {
    ds_dpy = dpy;
    ds_cmap = *cmap;
}

void
destroyDrawPlainString(proplist_t pl, WPluginData *func_data) {
    return;
}

/* FIX FIX FIX */
void
drawPlainString (proplist_t pl, Drawable d,
        int x, int y, unsigned width, unsigned height,
        char *text, WPluginData *func_data)
{
    XColor color1, color2, color3, color4;
    char *plcolor;
    int i, length;
    static int g;
    Pixmap drawbuffer;
    GC gc;
    WMFont *font;

    length = strlen(text);
    gc = func_data->array[2]; 
    font = func_data->array[3];


    /*
    printf("%d members\n",PLGetNumberOfElements(pl));
    for (i =0;i<7;i++) {
	    printf("%d %s\n",i,PLGetString(PLGetArrayElement(pl,i)));
    }
    return;
    */
    drawbuffer = XCreatePixmap(ds_dpy, d,
            width, height*4+6, DefaultDepth(ds_dpy,DefaultScreen(ds_dpy)));
    XCopyArea(ds_dpy, d, drawbuffer,gc,0,y-1,width,height,0,0);

    if (PLGetNumberOfElements(pl) > 5) {
        plcolor = PLGetArrayElement(pl, 5);
        if (getColor(PLGetString(plcolor),ds_cmap, &color3)) {
            XSetForeground(ds_dpy, gc, color3.pixel);

            if (font->notFontSet) {
                XSetFont(ds_dpy, gc, font->font.normal->fid);
                XDrawString(ds_dpy, drawbuffer, gc, x+3, font->y+3, text, length);
            } else {
                XmbDrawString(ds_dpy, drawbuffer, font->font.set, gc, x+4, y+4 + font->y,
                        text, length);
            }
        }
    }

    if (PLGetNumberOfElements(pl) > 4) {
        plcolor = PLGetArrayElement(pl, 4);
        if (getColor(PLGetString(plcolor),ds_cmap, &color1)) {
            XSetForeground(ds_dpy, gc, color1.pixel);

            if (font->notFontSet) {
                XSetFont(ds_dpy, gc, font->font.normal->fid);
                XDrawString(ds_dpy, drawbuffer, gc, x+1, font->y+1, text, length);
            } else {
                XmbDrawString(ds_dpy, drawbuffer, font->font.set, gc, x, y + font->y,
                        text, length);
            }
        }
    }

    if (PLGetNumberOfElements(pl) > 3) {
        plcolor = PLGetArrayElement(pl, 3);
        if (getColor(PLGetString(plcolor),ds_cmap, &color2)) {
            XSetForeground(ds_dpy, gc, color2.pixel);

            if (font->notFontSet) {
                XSetFont(ds_dpy, gc, font->font.normal->fid);
                XDrawString(ds_dpy, drawbuffer, gc, x,font->y, text, length);
            } else {
                XmbDrawString(ds_dpy, drawbuffer, font->font.set, gc, x-1, y-1 + font->y,
                        text, length);
            }
        }
    }

    /*
    plcolor = PLGetArrayElement(pl, 6);
    parse_xcolor(PLGetString(plcolor), &color4);
    */

    XCopyArea(ds_dpy, drawbuffer, d,gc,0,0,width,height,0,y-1);

    XFreePixmap(ds_dpy, drawbuffer);
}

#ifdef USE_FREETYPE

WMFreeTypeRImage *renderChar(FT_Face face, FT_ULong char_index, RColor *color) {
    FT_GlyphSlot slot;
    FT_Bitmap* bitmap;
    WMFreeTypeRImage *tmp_data;
    int index, x, y, i, error; /* error? no no no */

    tmp_data = malloc(sizeof(WMFreeTypeRImage));

    index = FT_Get_Char_Index(face, char_index);

    error = FT_Load_Glyph(face, index, FT_LOAD_DEFAULT);
    if (error) {
        _debug("error loading glyph\n");
        return NULL;
    }
    FT_Render_Glyph(face->glyph, ft_render_mode_normal);

    slot = face->glyph;
    bitmap = &slot->bitmap;
    tmp_data->advance_x = slot->advance.x;
    tmp_data->advance_y = slot->advance.y;
    tmp_data->top = slot->bitmap_top;
    tmp_data->left = slot->bitmap_left;
    if (bitmap->width > 0 && bitmap->rows > 0) {
        tmp_data->image = RCreateImage(bitmap->width, bitmap->rows, True);
    }
    else tmp_data->image = NULL;

    for (y=0; y < bitmap->rows; y++) {
        for (x=0; x < bitmap->width; x++) {
            color->alpha = bitmap->buffer[y * bitmap->width + x];
            ROperatePixel(tmp_data->image,
                    RCopyOperation, x, y, color);
        } 
    }
    return tmp_data;
}

void
widthOfFreeTypeString (unsigned char *text, int length, WPluginData *func_data,
        int *width, int *height, int *top) {
    WMFreeTypeData *data;
    RImage *rimg;
    int i;

    /* see framewin.c for the order of arguments (look in wPluginPackData) */
    data = ((WPluginData*)func_data->array[0])->array[2]; /* initialized data */

    if (width) *width = 0;
    if (height) *height = data->face->size->metrics.y_ppem;
    /* may finish height & top later if they really are neccessary */

    /* create temp for drawing */
    if (text)
    for (i = 0; i < length; i++) {
        if (!data->glyphs_array[text[i]]) {
            data->glyphs_array[text[i]] = renderChar(data->face, (FT_ULong)text[i], &data->color);
            data->glyphs_shadow_array[text[i]] = renderChar(data->face, (FT_ULong)text[i], &black_color);
        }
        if (data->glyphs_array[text[i]])
        if (data->glyphs_array[text[i]]->image) {
            if (width) *width += data->glyphs_array[text[i]]->advance_x >> 6;
        }
    }
}


/* drawFreeTypeString */
void initDrawFreeTypeString(proplist_t pl, WPluginData *init_data) {
    WMFreeTypeData *data;
    XColor xcolor;

    _debug("invoke initDrawFreeTypeString with init_data[3] %s\n",
            init_data->array[2]);
    _debug("%x is ds_dpy\n", ds_dpy);
    initDrawPlainString((Display *)init_data->array[0], (Colormap *)init_data->array[1]);
    _debug("then %x is ds_dpy\n", ds_dpy);

    /* set init_data[2] to array of RImage */

    /*
    * this would better to have sharable font system but
    * I want to see this more in WINGs though -- ]d
    */
    init_data->array[2] = malloc(sizeof(WMFreeTypeData));
    data = init_data->array[2];
    getColor(PLGetString(PLGetArrayElement(pl, 3)), ds_cmap, &xcolor);
    data->color.red = xcolor.red >> 8;
    data->color.green = xcolor.green >> 8;
    data->color.blue = xcolor.blue >> 8;
    
    data->glyphs_array = malloc(sizeof(WMFreeTypeRImage*) * MAX_GLYPHS);
    memset(data->glyphs_array, 0, sizeof(WMFreeTypeRImage*) * MAX_GLYPHS);
    data->glyphs_shadow_array = malloc(sizeof(WMFreeTypeRImage*) * MAX_GLYPHS);
    memset(data->glyphs_shadow_array, 0, sizeof(WMFreeTypeRImage*) * MAX_GLYPHS);
    data->strwidth = widthOfFreeTypeString;

    if (!rc) {
        RContextAttributes rcattr;

        rcattr.flags = RC_RenderMode | RC_ColorsPerChannel;
        rcattr.render_mode = RDitheredRendering;
        rcattr.colors_per_channel = 4;

        rc = RCreateContext(ds_dpy, DefaultScreen(ds_dpy), &rcattr);
    }

    /* case 1 -- no no case 2 yet :P */
    if (!inst_ft_library) {
        FT_Init_FreeType(&ft_library);
    }
    inst_ft_library++;

    FT_New_Face(ft_library, PLGetString(PLGetArrayElement(pl, 4)), 0, &data->face);
    FT_Set_Pixel_Sizes(data->face, 0, atoi(PLGetString(PLGetArrayElement(pl, 5))));
    _debug("initialize freetype library %d %d %d\n", ft_library, data->face, inst_ft_library);
}

void
destroyDrawFreeTypeString(proplist_t pl, WPluginData *func_data) {
    int i; /* error? no no no */
    WMFreeTypeData *data;

    data = (WMFreeTypeData *) func_data->array[2];
    for (i = 0; i < MAX_GLYPHS; i++) {
        if (data->glyphs_array[i]) {
            if (data->glyphs_array[i]->image)
                RDestroyImage(data->glyphs_array[i]->image);
            free(data->glyphs_array[i]);
        }
        if (data->glyphs_shadow_array[i]) {
            if (data->glyphs_shadow_array[i]->image)
                RDestroyImage(data->glyphs_shadow_array[i]->image);
            free(data->glyphs_shadow_array[i]);
        }
    }
    free(data->glyphs_array);
    free(data->glyphs_shadow_array);
    _debug("destroy freetype library %d %d %d\n", ft_library, data->face, inst_ft_library);
    FT_Done_Face(data->face);
    free(data);
    inst_ft_library--;
    if (!inst_ft_library) FT_Done_FreeType(ft_library);
}

void
logicalCombineArea(RImage *bg, RImage *image,
        int _sx, int _sy,
        int _sw, int _sh,
        int _dx, int _dy,
        int opaquueness) {

    /*
    if (_dx < 0) {
        _sx = -_dx;
        _sw = _sw + _dx;
        _dx = 0;
    }

    if (_dx + _sw > bg->width) {
        _sw = bg->width - _dx;
    }

    if (_dy < 0) {
        _sy = -_dy;
        _sh = _sh + _dy;
        _dy = 0;
    }

    if (_dy + _sh > bg->height) {
        _sh = bg->height - _dy;
    }
    */

    if (_sh > 0 && _sw > 0) {
        if (opaquueness) {
            RCombineAreaWithOpaqueness(bg, image, _sx, _sy,
                    _sw, _sh, _dx, _dy, opaquueness);
        } else {
            RCombineArea(bg, image, _sx, _sy,
                    _sw, _sh, _dx, _dy);
        }
    }
}


void
drawFreeTypeString (proplist_t pl, Drawable d,
        int x, int y, 
        unsigned char *text, int length, WPluginData *func_data) {
    WMFreeTypeData *data;
    RImage *rimg;
    int i, j, width, height;
    Pixmap pixmap;
    GC gc;
    int xwidth, xheight, dummy;
    Window wdummy;

    /* see framewin.c for the order of arguments (look in wPluginPackData) */
    data = ((WPluginData*)func_data->array[0])->array[2]; /* initialized data */
    if (func_data->array[1])
        pixmap = *(Pixmap *)func_data->array[1];
    gc = *(GC *)func_data->array[2];
    width = *(int *)func_data->array[4];
    height = *(int *)func_data->array[5];


    /* create temp for drawing */
    if (!func_data->array[1]) {
        XGetGeometry(ds_dpy, d, &wdummy, &dummy, &dummy, &xwidth, &xheight, &dummy, &dummy);
        pixmap = XCreatePixmap(ds_dpy, d, xwidth, xheight, DefaultDepth(ds_dpy, DefaultScreen(ds_dpy)));
        XClearWindow(ds_dpy, d);
        XCopyArea(ds_dpy, d, pixmap, gc, 0, 0, xwidth, xheight, 0, 0);
        rimg = RCreateImageFromDrawable(rc, pixmap, None);
        XFreePixmap(ds_dpy, pixmap);
    } else {
        rimg = RCreateImageFromDrawable(rc, pixmap, None);
    }

    if (rimg) {
        for (i = 0, j = x; i < strlen(text); i++) {
            if (!data->glyphs_array[text[i]]) {
                data->glyphs_array[text[i]] = renderChar(data->face, (FT_ULong)text[i], &data->color);
                data->glyphs_shadow_array[text[i]] = renderChar(data->face, (FT_ULong)text[i], &black_color);
            }
            if (data->glyphs_array[text[i]])
            if (data->glyphs_array[text[i]]->image) {
                int _dx, _dy, _sw, _sh;

                _dx = j + data->glyphs_array[text[i]]->left;
                _dy = (height + data->face->size->metrics.y_ppem)/2 -
                    data->glyphs_array[text[i]]->top;
                _sw = data->glyphs_array[text[i]]->image->width;
                _sh = data->glyphs_array[text[i]]->image->height;

                logicalCombineArea(rimg, data->glyphs_shadow_array[text[i]]->image,
                        0, 0, _sw, _sh, _dx-2, _dy+2, 100);
                logicalCombineArea(rimg, data->glyphs_array[text[i]]->image,
                        0, 0, _sw, _sh, _dx-3, _dy+1, 0);

                j += data->glyphs_array[text[i]]->advance_x >> 6;
            }
        }

        RConvertImage(rc, rimg, &pixmap);
        XCopyArea(ds_dpy, pixmap, d, gc, 0, 0, rimg->width, height, 0, y);
        XFreePixmap(ds_dpy, pixmap);
        RDestroyImage(rimg);

    }
}
 
#endif /* USE_FREETYPE */

/* core */

void
destroyDrawString (proplist_t pl, WPluginData *init_data) {
    if (strcmp(PLGetString(PLGetArrayElement(pl, 2)), "drawPlainString") == 0) 
        destroyDrawPlainString((Display *)init_data->array[0], NULL);
    else if (strcmp(PLGetString(PLGetArrayElement(pl, 2)), "drawFreeTypeString") == 0) 
        destroyDrawFreeTypeString(pl, init_data);
}

void
widthOfString (unsigned char *text, int length, WPluginData *func_data,
        int *width, int *height, int *top) {
    WMFreeTypeData *data;

    data = ((WPluginData*)func_data->array[0])->array[2];
    data->strwidth(text, length, func_data, width, height, top);
}

void
initDrawString (proplist_t pl, WPluginData *init_data) {
    _debug("invoke initDrawString: %s\n", PLGetString(PLGetArrayElement(pl, 2)));
    if (strcmp(PLGetString(PLGetArrayElement(pl, 2)), "drawPlainString") == 0) 
        initDrawPlainString((Display *)init_data->array[0], (Colormap *)init_data->array[1]);
#ifdef USE_FREETYPE
    else if (strcmp(PLGetString(PLGetArrayElement(pl, 2)), "drawFreeTypeString") == 0) 
        initDrawFreeTypeString(pl, init_data);
#endif
    _debug("finish initDrawString\n");
}

