// =============================================================================
// svg_rasterizer.cpp — Minimal Scratch SVG rasterizer for NDS
// =============================================================================
#include "svg_rasterizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// devkitARM doesn't expose strcasecmp — provide a simple replacement
static int svg_strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return (unsigned char)ca - (unsigned char)cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal RGBA framebuffer (32-bit, software rendered, then quantised)
// ─────────────────────────────────────────────────────────────────────────────
struct Rgba { uint8_t r, g, b, a; };

struct Fb {
    Rgba*  px;
    int    w, h;
    Fb(int w, int h) : w(w), h(h) { px = (Rgba*)calloc(w * h, sizeof(Rgba)); }
    ~Fb() { free(px); }
    Rgba& at(int x, int y) { return px[y * w + x]; }
    bool inBounds(int x, int y) const { return x >= 0 && x < w && y >= 0 && y < h; }
};

// Alpha-composite src over dst (Porter-Duff "over")
static void blendOver(Rgba& dst, const Rgba& src) {
    if (src.a == 0) return;
    if (src.a == 255 || dst.a == 0) { dst = src; return; }
    int a = src.a + (dst.a * (255 - src.a) / 255);
    if (a == 0) return;
    dst.r = (uint8_t)((src.r * src.a + dst.r * dst.a * (255 - src.a) / 255) / a);
    dst.g = (uint8_t)((src.g * src.a + dst.g * dst.a * (255 - src.a) / 255) / a);
    dst.b = (uint8_t)((src.b * src.a + dst.b * dst.a * (255 - src.a) / 255) / a);
    dst.a = (uint8_t)a;
}

// ─────────────────────────────────────────────────────────────────────────────
// Colour parsing
// ─────────────────────────────────────────────────────────────────────────────
static bool hexDigit(char c, int& v) {
    if (c >= '0' && c <= '9') { v = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { v = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { v = c - 'A' + 10; return true; }
    return false;
}

static Rgba parseColour(const char* s, float opacity = 1.0f) {
    if (!s || !*s) return {0,0,0,0};
    while (*s == ' ') s++;
    uint8_t alpha = (uint8_t)(opacity * 255);

    // #rgb or #rrggbb
    if (s[0] == '#') {
        int r=0,g=0,b=0,a,b2;
        if (strlen(s) == 4) {
            hexDigit(s[1],r); hexDigit(s[2],g); hexDigit(s[3],b);
            return {(uint8_t)(r*17),(uint8_t)(g*17),(uint8_t)(b*17),alpha};
        }
        if (strlen(s) >= 7) {
            hexDigit(s[1],r); hexDigit(s[2],a); r=r*16+a;
            hexDigit(s[3],g); hexDigit(s[4],b2); g=g*16+b2;
            hexDigit(s[5],b); hexDigit(s[6],a); b=b*16+a;
            return {(uint8_t)r,(uint8_t)g,(uint8_t)b,alpha};
        }
    }

    // rgb(r,g,b)
    if (strncmp(s, "rgb(", 4) == 0) {
        int r=0,g=0,b=0;
        sscanf(s+4, "%d,%d,%d", &r, &g, &b);
        return {(uint8_t)r,(uint8_t)g,(uint8_t)b,alpha};
    }

    // Named colours (subset used by Scratch)
    struct { const char* name; uint8_t r,g,b; } named[] = {
        {"white",255,255,255}, {"black",0,0,0}, {"none",0,0,0},
        {"red",255,0,0}, {"green",0,128,0}, {"blue",0,0,255},
        {"yellow",255,255,0}, {"orange",255,165,0}, {"purple",128,0,128},
        {"pink",255,192,203}, {"gray",128,128,128}, {"grey",128,128,128},
        {"cyan",0,255,255}, {"magenta",255,0,255}, {"lime",0,255,0},
        {"brown",165,42,42}, {"navy",0,0,128}, {"teal",0,128,128},
        {nullptr,0,0,0}
    };
    for (int i = 0; named[i].name; i++) {
        if (svg_strcasecmp(s, named[i].name) == 0) {
            uint8_t a2 = (svg_strcasecmp(s,"none")==0) ? 0 : alpha;
            return {named[i].r, named[i].g, named[i].b, a2};
        }
    }
    return {0,0,0,alpha};
}

// ─────────────────────────────────────────────────────────────────────────────
// Drawing primitives
// ─────────────────────────────────────────────────────────────────────────────

// Horizontal span fill with alpha blend
static void hLine(Fb& fb, int x0, int x1, int y, const Rgba& col) {
    if (y < 0 || y >= fb.h) return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 >= fb.w ? fb.w-1 : x1;
    for (int x = x0; x <= x1; x++) blendOver(fb.at(x, y), col);
}

// ── Polygon scan-line fill ────────────────────────────────────────────────────
static void fillPolygon(Fb& fb, const float* xs, const float* ys, int n, const Rgba& col) {
    if (n < 3) return;
    int ymin = (int)ys[0], ymax = (int)ys[0];
    for (int i = 1; i < n; i++) {
        int yi = (int)ys[i];
        if (yi < ymin) ymin = yi;
        if (yi > ymax) ymax = yi;
    }
    ymin = ymin < 0 ? 0 : ymin;
    ymax = ymax >= fb.h ? fb.h-1 : ymax;

    float* intersections = (float*)malloc((n+1)*sizeof(float));
    for (int y = ymin; y <= ymax; y++) {
        int cnt = 0;
        float fy = (float)y + 0.5f;
        for (int i = 0, j = n-1; i < n; j = i++) {
            float yi = ys[i], yj = ys[j];
            if ((yi <= fy && yj > fy) || (yj <= fy && yi > fy)) {
                intersections[cnt++] = xs[j] + (fy - yj) / (yi - yj) * (xs[i] - xs[j]);
            }
        }
        // Sort intersections
        for (int a = 0; a < cnt-1; a++)
            for (int b = a+1; b < cnt; b++)
                if (intersections[b] < intersections[a]) {
                    float t = intersections[a]; intersections[a]=intersections[b]; intersections[b]=t;
                }
        for (int k = 0; k+1 < cnt; k += 2)
            hLine(fb, (int)intersections[k], (int)intersections[k+1], y, col);
    }
    free(intersections);
}

// ── Circle / ellipse fill ─────────────────────────────────────────────────────
static void fillEllipse(Fb& fb, float cx, float cy, float rx, float ry, const Rgba& col) {
    int y0 = (int)(cy - ry); int y1 = (int)(cy + ry + 1.0f);
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 >= fb.h ? fb.h-1 : y1;
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        if (ry <= 0) continue;
        float dx2 = 1.0f - (dy*dy)/(ry*ry);
        if (dx2 < 0) continue;
        float dx = rx * sqrtf(dx2);
        hLine(fb, (int)(cx - dx), (int)(cx + dx), y, col);
    }
}

// ── Stroke (thick line) ───────────────────────────────────────────────────────
static void strokeLine(Fb& fb, float x0, float y0, float x1, float y1,
                        float sw, const Rgba& col) {
    float dx = x1-x0, dy = y1-y0;
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 0.001f) return;
    float nx = -dy/len * sw*0.5f, ny = dx/len * sw*0.5f;
    float xs[4] = {x0+nx, x1+nx, x1-nx, x0-nx};
    float ys[4] = {y0+ny, y1+ny, y1-ny, y0-ny};
    fillPolygon(fb, xs, ys, 4, col);
}

// ─────────────────────────────────────────────────────────────────────────────
// SVG mini-parser state
// ─────────────────────────────────────────────────────────────────────────────
struct Transform {
    float a,b,c,d,e,f; // [a c e; b d f; 0 0 1]
    Transform() : a(1),b(0),c(0),d(1),e(0),f(0) {}
    void apply(float x, float y, float& ox, float& oy) const {
        ox = a*x + c*y + e;
        oy = b*x + d*y + f;
    }
};

static Transform parseTransform(const char* s) {
    Transform t;
    if (!s) return t;
    // translate(tx[,ty])
    const char* p = strstr(s, "translate(");
    if (p) {
        float tx=0, ty=0;
        sscanf(p+10, "%f,%f", &tx, &ty);
        // try space separator too
        if (ty == 0) sscanf(p+10, "%f %f", &tx, &ty);
        t.e = tx; t.f = ty;
    }
    // scale(sx[,sy])
    p = strstr(s, "scale(");
    if (p) {
        float sx=1, sy=1;
        sscanf(p+6, "%f,%f", &sx, &sy);
        if (sy == 1) { sscanf(p+6, "%f %f", &sx, &sy); }
        if (sy == 1) sy = sx;
        t.a *= sx; t.d *= sy;
    }
    // matrix(a,b,c,d,e,f)
    p = strstr(s, "matrix(");
    if (p) {
        sscanf(p+7, "%f,%f,%f,%f,%f,%f", &t.a,&t.b,&t.c,&t.d,&t.e,&t.f);
    }
    return t;
}

// ─────────────────────────────────────────────────────────────────────────────
// Attribute extraction helpers
// ─────────────────────────────────────────────────────────────────────────────
// Find attr="..." or attr='...' in element text, copy value into buf (maxLen).
static bool getAttr(const char* elem, const char* attr, char* buf, int maxLen) {
    char search[64]; 
    snprintf(search, sizeof(search), "%s=", attr);
    const char* p = strstr(elem, search);
    if (!p) return false;
    p += strlen(search);
    char delim = (*p == '"') ? '"' : '\'';
    p++;
    int i = 0;
    while (*p && *p != delim && i < maxLen-1) buf[i++] = *p++;
    buf[i] = '\0';
    return true;
}

static float getAttrF(const char* elem, const char* attr, float def = 0.0f) {
    char buf[64]; 
    if (!getAttr(elem, attr, buf, sizeof(buf))) return def;
    return (float)atof(buf);
}

static Rgba getColourAttr(const char* elem, const char* attr, float opacity = 1.0f) {
    char buf[64]; 
    if (!getAttr(elem, attr, buf, sizeof(buf))) return {0,0,0,0};
    if (strcmp(buf, "none") == 0) return {0,0,0,0};
    return parseColour(buf, opacity);
}

// Parse inline style="..." for a property
static bool getStyleProp(const char* style, const char* prop, char* buf, int maxLen) {
    const char* p = strstr(style, prop);
    if (!p) return false;
    p += strlen(prop);
    while (*p == ' ' || *p == ':') p++;
    int i = 0;
    while (*p && *p != ';' && *p != '"' && i < maxLen-1) buf[i++] = *p++;
    buf[i] = '\0';
    return true;
}

// Resolve fill/stroke from either direct attribute or style="..."
static Rgba resolvePaint(const char* elem, const char* attr, float defaultOpacity) {
    // Check style attribute first
    char style[512] = {0};
    getAttr(elem, "style", style, sizeof(style));
    float opacity = defaultOpacity;

    // opacity from style
    char opBuf[32];
    if (getStyleProp(style, "fill-opacity", opBuf, sizeof(opBuf)) && strcmp(attr,"fill")==0)
        opacity = (float)atof(opBuf);
    if (getStyleProp(style, "stroke-opacity", opBuf, sizeof(opBuf)) && strcmp(attr,"stroke")==0)
        opacity = (float)atof(opBuf);

    // opacity attribute
    char opAttr[32];
    char opAttrName[32];
    snprintf(opAttrName, sizeof(opAttrName), "%s-opacity", attr);
    if (getAttr(elem, opAttrName, opAttr, sizeof(opAttr)))
        opacity = (float)atof(opAttr);

    // Value from style
    char colBuf[64];
    if (getStyleProp(style, attr, colBuf, sizeof(colBuf)))
        return parseColour(colBuf, opacity);

    // Value from attribute
    return getColourAttr(elem, attr, opacity);
}

// ─────────────────────────────────────────────────────────────────────────────
// SVG path parser  (M, L, H, V, C, S, Q, T, A, Z — absolute and relative)
// ─────────────────────────────────────────────────────────────────────────────
struct PathPt { float x, y; };

// Flatten a cubic Bézier into points (recursive subdivision)
static void flattenCubic(PathPt* pts, int& cnt, int maxPts,
                         float x0,float y0, float x1,float y1,
                         float x2,float y2, float x3,float y3, int depth=0) {
    if (depth > 6 || cnt >= maxPts-2) {
        if (cnt < maxPts) { pts[cnt++] = {x3,y3}; }
        return;
    }
    // Check if flat enough (chord-distance heuristic)
    float dx = x3-x0, dy = y3-y0;
    float d1 = fabsf((x1-x3)*dy - (y1-y3)*dx);
    float d2 = fabsf((x2-x3)*dy - (y2-y3)*dx);
    if ((d1+d2)*(d1+d2) < 0.5f*(dx*dx+dy*dy)) {
        if (cnt < maxPts) pts[cnt++] = {x3,y3};
        return;
    }
    float mx1=(x0+x1)*0.5f, my1=(y0+y1)*0.5f;
    float mx2=(x1+x2)*0.5f, my2=(y1+y2)*0.5f;
    float mx3=(x2+x3)*0.5f, my3=(y2+y3)*0.5f;
    float mx4=(mx1+mx2)*0.5f, my4=(my1+my2)*0.5f;
    float mx5=(mx2+mx3)*0.5f, my5=(my2+my3)*0.5f;
    float mx6=(mx4+mx5)*0.5f, my6=(my4+my5)*0.5f;
    flattenCubic(pts,cnt,maxPts, x0,y0, mx1,my1, mx4,my4, mx6,my6, depth+1);
    flattenCubic(pts,cnt,maxPts, mx6,my6, mx5,my5, mx3,my3, x3,y3, depth+1);
}

static void parsePath(Fb& fb, const char* dAttr,
                      const Rgba& fill, const Rgba& stroke, float sw,
                      const Transform& tr) {
    const int MAXPTS = 4096;
    PathPt* pts = (PathPt*)malloc(MAXPTS * sizeof(PathPt));
    int cnt = 0;

    float cx=0, cy=0, sx=0, sy=0; // current pos, subpath start
    float lc2x=0, lc2y=0;         // last control point (for S/T)
    char lastCmd = 0;

    const char* p = dAttr;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;

        char cmd = 0;
        if ((*p>='A'&&*p<='Z') || (*p>='a'&&*p<='z')) {
            cmd = *p++;
        } else {
            cmd = lastCmd; // implicit repetition
        }
        lastCmd = cmd;

        auto readF = [&]() -> float {
            while (*p == ' ' || *p == ',') p++;
            char* end;
            float v = (float)strtod(p, &end);
            p = end;
            return v;
        };

        char lc = cmd | 0x20; // lowercase
        bool rel = (cmd >= 'a' && cmd <= 'z');

        if (lc == 'm') {
            if (cnt > 2) {
                // Fill accumulated polygon
                float* xs = (float*)malloc(cnt*sizeof(float));
                float* ys = (float*)malloc(cnt*sizeof(float));
                for (int i=0;i<cnt;i++) { float ox,oy; tr.apply(pts[i].x,pts[i].y,ox,oy); xs[i]=ox; ys[i]=oy; }
                if (fill.a > 0) fillPolygon(fb, xs, ys, cnt, fill);
                free(xs); free(ys);
                cnt = 0;
            }
            float x=readF(), y=readF();
            if (rel) { cx+=x; cy+=y; } else { cx=x; cy=y; }
            sx=cx; sy=cy;
            pts[cnt++] = {cx,cy};
            lastCmd = rel ? 'l' : 'L';
        }
        else if (lc == 'l') {
            float x=readF(), y=readF();
            if (rel) { cx+=x; cy+=y; } else { cx=x; cy=y; }
            if (cnt<MAXPTS) pts[cnt++]={cx,cy};
        }
        else if (lc == 'h') {
            float x=readF();
            if (rel) cx+=x; else cx=x;
            if (cnt<MAXPTS) pts[cnt++]={cx,cy};
        }
        else if (lc == 'v') {
            float y=readF();
            if (rel) cy+=y; else cy=y;
            if (cnt<MAXPTS) pts[cnt++]={cx,cy};
        }
        else if (lc == 'c') {
            float x1=readF(),y1=readF(),x2=readF(),y2=readF(),x3=readF(),y3=readF();
            if (rel){x1+=cx;y1+=cy;x2+=cx;y2+=cy;x3+=cx;y3+=cy;}
            lc2x=x2; lc2y=y2;
            flattenCubic(pts,cnt,MAXPTS, cx,cy, x1,y1, x2,y2, x3,y3);
            cx=x3; cy=y3;
        }
        else if (lc == 's') {
            float rx1=2*cx-lc2x, ry1=2*cy-lc2y;
            float x2=readF(),y2=readF(),x3=readF(),y3=readF();
            if (rel){x2+=cx;y2+=cy;x3+=cx;y3+=cy;}
            lc2x=x2; lc2y=y2;
            flattenCubic(pts,cnt,MAXPTS, cx,cy, rx1,ry1, x2,y2, x3,y3);
            cx=x3; cy=y3;
        }
        else if (lc == 'q') {
            float x1=readF(),y1=readF(),x2=readF(),y2=readF();
            if (rel){x1+=cx;y1+=cy;x2+=cx;y2+=cy;}
            // Elevate quadratic to cubic
            float cx1=cx+(2.0f/3.0f)*(x1-cx), cy1=cy+(2.0f/3.0f)*(y1-cy);
            float cx2=x2+(2.0f/3.0f)*(x1-x2), cy2=y2+(2.0f/3.0f)*(y1-y2);
            lc2x=x1; lc2y=y1;
            flattenCubic(pts,cnt,MAXPTS, cx,cy, cx1,cy1, cx2,cy2, x2,y2);
            cx=x2; cy=y2;
        }
        else if (lc == 't') {
            float rx1=2*cx-lc2x, ry1=2*cy-lc2y;
            float x2=readF(),y2=readF();
            if (rel){x2+=cx;y2+=cy;}
            float cx1=cx+(2.0f/3.0f)*(rx1-cx), cy1=cy+(2.0f/3.0f)*(ry1-cy);
            float cx2=x2+(2.0f/3.0f)*(rx1-x2), cy2=y2+(2.0f/3.0f)*(ry1-y2);
            lc2x=rx1; lc2y=ry1;
            flattenCubic(pts,cnt,MAXPTS, cx,cy, cx1,cy1, cx2,cy2, x2,y2);
            cx=x2; cy=y2;
        }
        else if (lc == 'a') {
            // Approximate arc with line segments
            float rx=readF(),ry=readF(),rot=readF();
            float largeArc=readF(), sweep=readF();
            float ex=readF(), ey=readF();
            if (rel){ex+=cx;ey+=cy;}
            // Simple: subdivide into ~16 line segments
            int steps=16;
            for (int si=1;si<=steps;si++) {
                float t=(float)si/steps;
                float x=cx+(ex-cx)*t, y=cy+(ey-cy)*t;
                if (cnt<MAXPTS) pts[cnt++]={x,y};
            }
            cx=ex; cy=ey;
            (void)rx;(void)ry;(void)rot;(void)largeArc;(void)sweep;
        }
        else if (lc == 'z') {
            if (cnt > 2) {
                if (cnt<MAXPTS) pts[cnt++]={sx,sy};
                float* xs=(float*)malloc(cnt*sizeof(float));
                float* ys=(float*)malloc(cnt*sizeof(float));
                for (int i=0;i<cnt;i++){float ox,oy;tr.apply(pts[i].x,pts[i].y,ox,oy);xs[i]=ox;ys[i]=oy;}
                if (fill.a>0) fillPolygon(fb,xs,ys,cnt,fill);
                // Stroke outline
                if (stroke.a>0 && sw>0) {
                    for (int i=0;i<cnt-1;i++)
                        strokeLine(fb,xs[i],ys[i],xs[i+1],ys[i+1],sw,stroke);
                }
                free(xs); free(ys);
            }
            cnt=0;
            cx=sx; cy=sy;
        }
    }
    // Flush remaining open path as stroke only
    if (cnt>1 && stroke.a>0 && sw>0) {
        float* xs=(float*)malloc(cnt*sizeof(float));
        float* ys=(float*)malloc(cnt*sizeof(float));
        for (int i=0;i<cnt;i++){float ox,oy;tr.apply(pts[i].x,pts[i].y,ox,oy);xs[i]=ox;ys[i]=oy;}
        for (int i=0;i<cnt-1;i++)
            strokeLine(fb,xs[i],ys[i],xs[i+1],ys[i+1],sw,stroke);
        free(xs); free(ys);
    }
    free(pts);
}

// ─────────────────────────────────────────────────────────────────────────────
// Draw a single SVG element
// ─────────────────────────────────────────────────────────────────────────────
static void drawElement(Fb& fb, const char* tag, const char* elem,
                        const Transform& parentTr, float scaleX, float scaleY) {
    // Transform
    char trStr[256]="";
    getAttr(elem, "transform", trStr, sizeof(trStr));
    Transform tr = parseTransform(trStr[0] ? trStr : nullptr);
    // Compose with parent: apply parent first
    Transform combined;
    combined.a = parentTr.a*tr.a + parentTr.c*tr.b;
    combined.b = parentTr.b*tr.a + parentTr.d*tr.b;
    combined.c = parentTr.a*tr.c + parentTr.c*tr.d;
    combined.d = parentTr.b*tr.c + parentTr.d*tr.d;
    combined.e = parentTr.a*tr.e + parentTr.c*tr.f + parentTr.e;
    combined.f = parentTr.b*tr.e + parentTr.d*tr.f + parentTr.f;

    // Scale-aware stroke width
    float sw = getAttrF(elem, "stroke-width", 1.0f) * scaleX;
    char swStyle[32];
    char style[512]=""; getAttr(elem,"style",style,sizeof(style));
    if (getStyleProp(style,"stroke-width",swStyle,sizeof(swStyle)))
        sw = (float)atof(swStyle) * scaleX;

    Rgba fill   = resolvePaint(elem, "fill",   1.0f);
    Rgba stroke = resolvePaint(elem, "stroke", 1.0f);

    // Default fill=black if nothing specified (SVG spec)
    // But only if neither fill attr nor style present
    {
        char tmp[16];
        char styleStr[512]="";
        getAttr(elem,"style",styleStr,sizeof(styleStr));
        bool hasFill = getAttr(elem,"fill",tmp,sizeof(tmp)) ||
                       getStyleProp(styleStr,"fill",tmp,sizeof(tmp));
        if (!hasFill) fill = {0,0,0,255};
    }

    if (strcmp(tag,"rect")==0) {
        float x=getAttrF(elem,"x"), y=getAttrF(elem,"y");
        float w=getAttrF(elem,"width"), h=getAttrF(elem,"height");
        float rx=getAttrF(elem,"rx"), ry=getAttrF(elem,"ry");
        if (ry==0) ry=rx;
        // Simple rect (ignore rounded corners for now)
        float xs[4]={x,x+w,x+w,x}, ys[4]={y,y,y+h,y+h};
        float oxs[4], oys[4];
        for (int i=0;i<4;i++) combined.apply(xs[i],ys[i],oxs[i],oys[i]);
        if (fill.a>0) fillPolygon(fb,oxs,oys,4,fill);
        if (stroke.a>0 && sw>0) {
            for (int i=0;i<4;i++) strokeLine(fb,oxs[i],oys[i],oxs[(i+1)%4],oys[(i+1)%4],sw,stroke);
        }
        (void)rx;(void)ry;
    }
    else if (strcmp(tag,"circle")==0 || strcmp(tag,"ellipse")==0) {
        float cx, cy, rx, ry;
        if (strcmp(tag,"circle")==0) {
            cx=getAttrF(elem,"cx"); cy=getAttrF(elem,"cy");
            rx=ry=getAttrF(elem,"r");
        } else {
            cx=getAttrF(elem,"cx"); cy=getAttrF(elem,"cy");
            rx=getAttrF(elem,"rx"); ry=getAttrF(elem,"ry");
        }
        float ocx,ocy; combined.apply(cx,cy,ocx,ocy);
        fillEllipse(fb, ocx, ocy, rx*scaleX, ry*scaleY, fill);
        // Stroke: approximate with thin ring
        if (stroke.a>0 && sw>0) {
            // Draw as outline polygon approximation
            int steps=32;
            float* xs=(float*)malloc(steps*sizeof(float));
            float* ys=(float*)malloc(steps*sizeof(float));
            for (int i=0;i<steps;i++) {
                float angle=2.0f*M_PI*i/steps;
                float px=cx+rx*cosf(angle), py=cy+ry*sinf(angle);
                combined.apply(px,py,xs[i],ys[i]);
            }
            for (int i=0;i<steps;i++)
                strokeLine(fb,xs[i],ys[i],xs[(i+1)%steps],ys[(i+1)%steps],sw,stroke);
            free(xs); free(ys);
        }
    }
    else if (strcmp(tag,"line")==0) {
        float x1=getAttrF(elem,"x1"),y1=getAttrF(elem,"y1");
        float x2=getAttrF(elem,"x2"),y2=getAttrF(elem,"y2");
        float ox1,oy1,ox2,oy2;
        combined.apply(x1,y1,ox1,oy1); combined.apply(x2,y2,ox2,oy2);
        if (stroke.a>0 && sw>0) strokeLine(fb,ox1,oy1,ox2,oy2,sw,stroke);
    }
    else if (strcmp(tag,"polyline")==0 || strcmp(tag,"polygon")==0) {
        char pts[2048]="";
        getAttr(elem,"points",pts,sizeof(pts));
        float xs[256],ys[256]; int n=0;
        const char* p=pts;
        while (*p && n<256) {
            while(*p==' '||*p==',') p++;
            if(!*p) break;
            char* e; xs[n]=(float)strtod(p,&e); p=e;
            while(*p==' '||*p==',') p++;
            ys[n]=(float)strtod(p,&e); p=e;
            float ox,oy; combined.apply(xs[n],ys[n],ox,oy); xs[n]=ox; ys[n]=oy;
            n++;
        }
        if (n>2 && fill.a>0) fillPolygon(fb,xs,ys,n,fill);
        if (stroke.a>0 && sw>0) {
            int end = (strcmp(tag,"polygon")==0) ? n : n-1;
            for (int i=0;i<end;i++)
                strokeLine(fb,xs[i],ys[i],xs[(i+1)%n],ys[(i+1)%n],sw,stroke);
        }
    }
    else if (strcmp(tag,"path")==0) {
        char d[8192]="";
        getAttr(elem,"d",d,sizeof(d));
        parsePath(fb, d, fill, stroke, sw, combined);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Quantise RGBA framebuffer to 8bpp indexed + RGB555 palette
// ─────────────────────────────────────────────────────────────────────────────
static void quantise(const Fb& fb, SvgImage& out) {
    // Index 0 = transparent
    out.palette[0] = 0;
    out.palCount   = 1;

    for (int y = 0; y < fb.h; y++) {
        for (int x = 0; x < fb.w; x++) {
            const Rgba& px = fb.px[y*fb.w+x];
            if (px.a < 128) {
                out.pixels[y*fb.w+x] = 0;
                continue;
            }
            uint16_t c15 = ((px.r>>3)) | ((px.g>>3)<<5) | ((px.b>>3)<<10);
            // Find in palette
            int idx = 0;
            for (int i = 1; i < out.palCount; i++) {
                if (out.palette[i] == c15) { idx = i; break; }
            }
            if (idx == 0) {
                if (out.palCount < 256) {
                    out.palette[out.palCount] = c15;
                    idx = out.palCount++;
                } else {
                    // Find nearest
                    int best=1; int bestD=0x7FFFFFFF;
                    int r=(c15&0x1F)<<3, g=((c15>>5)&0x1F)<<3, b=((c15>>10)&0x1F)<<3;
                    for (int i=1;i<256;i++) {
                        int pr=(out.palette[i]&0x1F)<<3;
                        int pg=((out.palette[i]>>5)&0x1F)<<3;
                        int pb=((out.palette[i]>>10)&0x1F)<<3;
                        int d=(r-pr)*(r-pr)+(g-pg)*(g-pg)+(b-pb)*(b-pb);
                        if (d<bestD){bestD=d;best=i;}
                    }
                    idx=best;
                }
            }
            out.pixels[y*fb.w+x] = (uint8_t)idx;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main rasterizer — parse SVG and render into framebuffer
// ─────────────────────────────────────────────────────────────────────────────
static bool rasterizeCore(const char* svg, size_t svgLen, SvgImage& out, int dstW, int dstH) {
    out.ok = false;
    if (!svg || svgLen == 0) return false;
    if (dstW > SVG_MAX_W) dstW = SVG_MAX_W;
    if (dstH > SVG_MAX_H) dstH = SVG_MAX_H;

    out.width  = dstW;
    out.height = dstH;
    memset(out.pixels, 0, sizeof(out.pixels));
    memset(out.palette,0, sizeof(out.palette));
    out.palCount = 0;

    // Parse viewBox
    float vx=0, vy=0, vw=0, vh=0;
    const char* vbp = strstr(svg, "viewBox");
    if (vbp) {
        vbp = strchr(vbp, '"'); 
        if (!vbp) vbp = strchr(strstr(svg,"viewBox"), '\'');
        if (vbp) {
            vbp++;
            sscanf(vbp, "%f %f %f %f", &vx, &vy, &vw, &vh);
            if (vw == 0) sscanf(vbp, "%f,%f,%f,%f", &vx, &vy, &vw, &vh);
        }
    }
    // Fallback: use width/height attributes
    if (vw == 0) {
        const char* wp = strstr(svg, "width=");
        const char* hp = strstr(svg, "height=");
        if (wp) {
            const char* wpq = strchr(wp, '"');
            if (!wpq) wpq = strchr(wp, '\'');
            if (wpq) vw = (float)atof(wpq + 1);
        }
        if (hp) {
            const char* hpq = strchr(hp, '"');
            if (!hpq) hpq = strchr(hp, '\'');
            if (hpq) vh = (float)atof(hpq + 1);
        }
    }
    if (vw <= 0) vw = 480; // Scratch default stage width
    if (vh <= 0) vh = 360;

    float scaleX = (float)dstW / vw;
    float scaleY = (float)dstH / vh;

    Fb fb(dstW, dstH);

    // Build a root transform: translate(-vx,-vy), scale(scaleX, scaleY)
    Transform rootTr;
    rootTr.a = scaleX; rootTr.d = scaleY;
    rootTr.e = -vx * scaleX; rootTr.f = -vy * scaleY;

    // Walk elements (simple linear scan — no full XML tree needed for Scratch SVGs)
    const char* TAGS[] = {"rect","circle","ellipse","line","polyline","polygon","path",nullptr};

    const char* p = svg;
    while (*p) {
        // Find next '<'
        const char* lt = strchr(p, '<');
        if (!lt) break;
        p = lt + 1;

        // Skip comments and declarations
        if (strncmp(p, "!--", 3) == 0) { const char* e=strstr(p,"-->"); p=e?e+3:p+3; continue; }
        if (*p == '?' || *p == '!') { const char* e=strchr(p,'>'); p=e?e+1:p+1; continue; }
        if (*p == '/') { const char* e=strchr(p,'>'); p=e?e+1:p+1; continue; }

        // Read tag name
        char tagName[32]="";
        int ti=0;
        while (*p && *p!=' ' && *p!='>' && *p!='/' && ti<31) tagName[ti++]=*p++;
        tagName[ti]='\0';

        // Find end of element
        const char* elemEnd = strchr(p, '>');
        if (!elemEnd) break;

        // Copy element text for attribute parsing
        int eLen = (int)(elemEnd - lt + 2);
        if (eLen > 16384) { p = elemEnd+1; continue; }
        char* elem = (char*)malloc(eLen+1);
        strncpy(elem, lt, eLen);
        elem[eLen] = '\0';

        // Check if it's a drawable tag
        for (int ti2 = 0; TAGS[ti2]; ti2++) {
            if (strcmp(tagName, TAGS[ti2]) == 0) {
                drawElement(fb, tagName, elem, rootTr, scaleX, scaleY);
                break;
            }
        }

        // Handle <g> groups: pass through (child elements processed naturally)
        free(elem);
        p = elemEnd + 1;
    }

    quantise(fb, out);
    out.ok = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────
bool svgRasterize(const char* path, SvgImage& out, int dstW, int dstH) {
    // SVG rasterization is too slow for NDS ARM9 at load time.
    // Render a placeholder pink square instead.
    if (dstW > SVG_MAX_W) dstW = SVG_MAX_W;
    if (dstH > SVG_MAX_H) dstH = SVG_MAX_H;
    out.width    = dstW;
    out.height   = dstH;
    out.palCount = 2;
    out.ok       = true;
    memset(out.pixels, 1, sizeof(out.pixels));   // all index 1
    out.palette[0] = 0;                           // transparent
    out.palette[1] = (uint16_t)((31) | (0<<5) | (31<<10)); // pink RGB555
    return true;
}

bool svgRasterizeString(const char* svgData, size_t len,
                        SvgImage& out, int dstW, int dstH) {
    (void)svgData; (void)len;
    return svgRasterize(nullptr, out, dstW, dstH);
}
