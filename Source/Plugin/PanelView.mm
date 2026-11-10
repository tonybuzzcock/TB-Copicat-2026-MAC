// PanelView.mm
// The custom panel for TB Copicat IC300: an original design laid out like the
// echo unit's controls. Opaque silver, black lettering, grey knurled knobs with
// red tops and a white position line, three latching head buttons, rocker switches for Tone
// and Echo in/out, and a strip of tape-transport controls, presets and the
// readout along the bottom.
//
// Cocoa, manual retain/release, and only APIs that exist in Mac OS X 10.8.
// Everything is drawn in code; no image files.

#import <Cocoa/Cocoa.h>

#include "Editor.h"
#include "Params.h"
#include "Presets.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace tbcc;

namespace
{

enum CtlType { kCtlKnob, kCtlPush, kCtlRocker, kCtlSmallKnob };

struct Ctl
{
    int param;
    int type;
    CGFloat cx, cy, r;
    const char* label;
    const char* posA;      // selector / rocker / slide labels
    const char* posB;
    const char* posC;
};

const CGFloat kStripTop = 470.0;

// Main row: Gain on the left, the tape loop over the head buttons in the
// middle, Swell and Sustain on the right. Tone and Echo below. Then the strip.
const Ctl kCtls[] = {
    { kGain,    kCtlKnob,      130, 200, 30, "GAIN",     0, 0, 0 },
    { kSwell,   kCtlKnob,      620, 200, 26, "SWELL",    0, 0, 0 },
    { kSustain, kCtlKnob,      730, 200, 26, "SUSTAIN",  0, 0, 0 },
    { kHead1,   kCtlPush,      330, 215, 20, "HEAD 1",   "1", 0, 0 },
    { kHead2,   kCtlPush,      400, 215, 20, "HEAD 2",   "2", 0, 0 },
    { kHead3,   kCtlPush,      470, 215, 20, "HEAD 3",   "3", 0, 0 },
    { kTone,    kCtlRocker,    125, 375, 0,  "TONE",     "OFF", "ON", 0 },
    { kEcho,    kCtlRocker,    665, 375, 0,  "ECHO",     "OUT", "IN", 0 },
    // Plugin strip: transport knobs, presets underneath
    { kSpeed,   kCtlSmallKnob, 210, 512, 15, "SPEED",    "SLOW", "FAST", 0 },
    { kWow,     kCtlSmallKnob, 330, 512, 15, "WOW & FLUTTER", "0", "10", 0 },
    { kTapeAge, kCtlSmallKnob, 450, 512, 15, "TAPE AGE", "NEW", "OLD", 0 },
    { kOutput,  kCtlSmallKnob, 570, 512, 15, "OUTPUT",   "-24", "+24", 0 },
};

// Preset row under the strip knobs: SAVE and slots 1..8; the readout to the right
const NSRect kSaveRect = { { 180.0, 562.0 }, { 54.0, 24.0 } };
const CGFloat kSlotX0 = 244.0, kSlotPitch = 27.0, kSlotW = 24.0, kSlotY = 562.0, kSlotH = 24.0;
const NSRect kReadoutRect = { { 490.0, 552.0 }, { 296.0, 40.0 } };

// Animated tape loop, centred above the head buttons. Drawn in its own
// coordinates (reel and capstan below) and scaled into place.
const CGFloat kReelX = 597.0, kReelY = 59.0, kReelR = 18.0;
const CGFloat kCapX = 688.0, kCapY = 59.0, kCapR = 9.0;
const CGFloat kSpoolSrcX = 640.0, kSpoolSrcY = 59.0;          // centre of the loop drawing
const CGFloat kSpoolX = 400.0, kSpoolY = 115.0, kSpoolScale = 1.6;
const NSRect kSpoolRect = { { 400.0 - 130.0, 115.0 - 47.0 }, { 260.0, 94.0 } };
const double kTapePxPerSec = 36.0;     // on-screen tape speed at normal Speed
const double kFrameRate = 30.0;

inline NSRect slotRect (int k) { return NSMakeRect (kSlotX0 + k * kSlotPitch, kSlotY, kSlotW, kSlotH); }
const int kNumCtls = (int) (sizeof (kCtls) / sizeof (kCtls[0]));

// Pointer angle (radians, 0 = up, clockwise positive) for a 0..1 value.
inline CGFloat valueAngle (CGFloat v) { return (CGFloat) ((-150.0 + 300.0 * v) * M_PI / 180.0); }

// Point at angle a (0 = up, clockwise) and radius r around (cx, cy) in flipped coordinates.
inline NSPoint polar (CGFloat cx, CGFloat cy, CGFloat r, CGFloat a)
{
    return NSMakePoint (cx + r * std::sin (a), cy - r * std::cos (a));
}

NSColor* rgb (int r, int g, int b) { return [NSColor colorWithCalibratedRed: r / 255.0 green: g / 255.0 blue: b / 255.0 alpha: 1.0]; }

NSFont* boldFont (CGFloat size)
{
    NSFont* f = [NSFont fontWithName: @"Helvetica-Bold" size: size];
    return f ? f : [NSFont boldSystemFontOfSize: size];
}

NSFont* plainFont (CGFloat size)
{
    NSFont* f = [NSFont fontWithName: @"Helvetica" size: size];
    return f ? f : [NSFont systemFontOfSize: size];
}

// First installed font from a list of PostScript names, else the fallback.
NSFont* firstFont (const char* const* names, int count, CGFloat size, NSFont* fallback)
{
    for (int i = 0; i < count; ++i)
    {
        NSFont* f = [NSFont fontWithName: [NSString stringWithUTF8String: names[i]] size: size];
        if (f) return f;
    }
    return fallback;
}

// Eurostile for the TB mark when it is installed (it cannot be bundled), else Helvetica.
NSFont* markBoldFont (CGFloat size)
{
    const char* const names[] = { "Eurostile-Bold", "EurostileBold", "Eurostile-Heavy", "EurostileLTStd-Bold", "EurostileT-Bold" };
    return firstFont (names, 5, size, boldFont (size));
}

NSFont* markPlainFont (CGFloat size)
{
    const char* const names[] = { "Eurostile", "Eurostile-Regular", "EurostileLTStd", "EurostileT-Regular" };
    return firstFont (names, 4, size, plainFont (size));
}

void drawText (const char* s, NSFont* font, NSColor* color, CGFloat cx, CGFloat cy)
{
    NSString* str = [NSString stringWithUTF8String: s];
    NSDictionary* attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                             font, NSFontAttributeName, color, NSForegroundColorAttributeName, nil];
    const NSSize sz = [str sizeWithAttributes: attrs];
    [str drawAtPoint: NSMakePoint (cx - sz.width * 0.5, cy - sz.height * 0.5) withAttributes: attrs];
}

void drawTextLeft (const char* s, NSFont* font, NSColor* color, CGFloat x, CGFloat cy)
{
    NSString* str = [NSString stringWithUTF8String: s];
    NSDictionary* attrs = [NSDictionary dictionaryWithObjectsAndKeys:
                             font, NSFontAttributeName, color, NSForegroundColorAttributeName, nil];
    const NSSize sz = [str sizeWithAttributes: attrs];
    [str drawAtPoint: NSMakePoint (x, cy - sz.height * 0.5) withAttributes: attrs];
}

} // namespace

//==============================================================================
@interface TBCopicatPanelView : NSView
{
    EditorHost* host;
    float shown[kNumParams];
    NSTimer* timer;
    int dragCtl;
    CGFloat dragStartY, dragStartValue;
    int readoutParam;
    bool saveMode;
    int activePreset;
    bool presetUsed[kNumPresetSlots];
    char status[64];
    double tapePx;       // how far the tape has moved, in pixels
    double wobPh;
}
- (id) initWithFrame: (NSRect) frame host: (EditorHost*) h;
- (void) shutdown;
@end

@implementation TBCopicatPanelView

- (id) initWithFrame: (NSRect) frame host: (EditorHost*) h
{
    self = [super initWithFrame: frame];
    if (self)
    {
        host = h;
        for (int i = 0; i < kNumParams; ++i) shown[i] = host->getParam (i);
        dragCtl = -1;
        readoutParam = -1;
        saveMode = false;
        activePreset = -1;
        status[0] = 0;
        tapePx = 0.0;
        wobPh = 0.0;
        host->refreshPresets();
        for (int k = 0; k < kNumPresetSlots; ++k) presetUsed[k] = host->presetExists (k);
        timer = [[NSTimer timerWithTimeInterval: 1.0 / kFrameRate target: self selector: @selector (tick:)
                                       userInfo: nil repeats: YES] retain];
        [[NSRunLoop currentRunLoop] addTimer: timer forMode: NSDefaultRunLoopMode];
        [[NSRunLoop currentRunLoop] addTimer: timer forMode: NSEventTrackingRunLoopMode];
    }
    return self;
}

- (void) shutdown
{
    if (timer) { [timer invalidate]; [timer release]; timer = nil; }
    host = 0;
}

- (void) dealloc
{
    [self shutdown];
    [super dealloc];
}

- (BOOL) isFlipped { return YES; }
- (BOOL) isOpaque { return YES; }
- (BOOL) acceptsFirstMouse: (NSEvent*) e { (void) e; return YES; }

// Host-side changes (automation, presets) redraw the panel.
- (void) tick: (NSTimer*) t
{
    (void) t;
    if (! host) return;
    bool changed = false;
    for (int i = 0; i < kNumParams; ++i)
    {
        const float v = host->getParam (i);
        if (v != shown[i]) { shown[i] = v; changed = true; }
    }
    if (changed) [self setNeedsDisplay: YES];

    // Move the tape: Speed sets the pace, Wow & Flutter makes it waver
    const double speed = std::pow (1.35, 2.0 * shown[kSpeed] - 1.0);
    wobPh += 0.55 / kFrameRate;
    if (wobPh >= 1.0) wobPh -= 1.0;
    const double wob = 1.0 + 0.06 * shown[kWow] * std::sin (2.0 * M_PI * wobPh);
    tapePx += kTapePxPerSec * speed * wob / kFrameRate;
    if (tapePx > 1.0e6) tapePx -= 1.0e6;
    if (! changed) [self setNeedsDisplayInRect: kSpoolRect];
}

//------------------------------------------------------------------------------
// Drawing

- (void) drawBackground
{
    const NSRect b = [self bounds];

    // Opaque silver faceplate
    NSGradient* face = [[[NSGradient alloc] initWithStartingColor: rgb (222, 224, 227)
                                                      endingColor: rgb (184, 188, 192)] autorelease];
    [face drawInRect: NSMakeRect (0, 0, b.size.width, kStripTop) angle: 90.0];

    // Plugin strip, a slightly darker silver
    NSGradient* strip = [[[NSGradient alloc] initWithStartingColor: rgb (196, 199, 203)
                                                       endingColor: rgb (166, 170, 175)] autorelease];
    [strip drawInRect: NSMakeRect (0, kStripTop, b.size.width, b.size.height - kStripTop) angle: 90.0];

    [rgb (40, 40, 42) set];
    NSRectFill (NSMakeRect (0, kStripTop, b.size.width, 2.0));
    [rgb (238, 240, 242) set];
    NSRectFill (NSMakeRect (0, kStripTop + 2.0, b.size.width, 1.0));

    // Fine border around the faceplate
    NSBezierPath* border = [NSBezierPath bezierPathWithRect: NSInsetRect (NSMakeRect (0, 0, b.size.width, kStripTop), 6.5, 6.5)];
    [rgb (70, 72, 75) set];
    [border setLineWidth: 1.0];
    [border stroke];

    // Section dividers and titles: three columns, a rule above the switch row
    NSColor* ink = [NSColor blackColor];
    [rgb (60, 62, 65) set];
    {
        const CGFloat lines[][4] = {
            { 255.5, 22, 255.5, 456 }, { 545.5, 22, 545.5, 456 },
            { 22, 300.5, 255, 300.5 }, { 256, 300.5, 545, 300.5 }, { 546, 300.5, 778, 300.5 },
        };
        for (int i = 0; i < 5; ++i)
        {
            NSBezierPath* p = [NSBezierPath bezierPath];
            [p moveToPoint: NSMakePoint (lines[i][0], lines[i][1])];
            [p lineToPoint: NSMakePoint (lines[i][2], lines[i][3])];
            [p setLineWidth: 1.0];
            [p stroke];
        }
    }
    drawText ("INPUT", boldFont (12.0), ink, 130, 26);
    drawText ("PLAYBACK HEADS", boldFont (12.0), ink, 400, 26);
    drawText ("ECHO", boldFont (12.0), ink, 675, 26);

    // Nameplate in the middle of the switch row
    drawText ("TAPE ECHO", boldFont (15.0), ink, 400, 366);
    drawText ("IC 300", plainFont (13.0), ink, 400, 388);

    // Owner's mark in the strip's corner: red TB over "Electronic", straight on the silver
    {
        const NSRect plate = NSMakeRect (24.5, 507.5, 100.0, 56.0);
        drawText ("TB", markBoldFont (28.0), rgb (200, 30, 30), NSMidX (plate), plate.origin.y + 21.0);
        drawText ("Electronic", markPlainFont (12.0), ink, NSMidX (plate), plate.origin.y + 44.0);
    }
}

- (void) drawScaleForCtl: (const Ctl&) c
{
    NSColor* ink = [NSColor blackColor];
    for (int i = 0; i < 11; ++i)
    {
        const CGFloat a = valueAngle (i / 10.0);
        NSBezierPath* t = [NSBezierPath bezierPath];
        [t moveToPoint: polar (c.cx, c.cy, c.r + 4.0, a)];
        [t lineToPoint: polar (c.cx, c.cy, c.r + 9.0, a)];
        [t setLineWidth: 1.2];
        [ink set];
        [t stroke];

        char num[4];
        std::snprintf (num, sizeof (num), "%d", i);
        const NSPoint p = polar (c.cx, c.cy, c.r + 15.0, a);
        drawText (num, plainFont (8.5), ink, p.x, p.y);
    }
}

// Grey knurled knob, red top, white position line.
- (void) drawKnobAt: (NSPoint) ctr radius: (CGFloat) r value: (CGFloat) v
{
    // Drop shadow
    [NSGraphicsContext saveGraphicsState];
    NSShadow* sh = [[[NSShadow alloc] init] autorelease];
    [sh setShadowOffset: NSMakeSize (1.5, -2.5)];
    [sh setShadowBlurRadius: 4.0];
    [sh setShadowColor: [NSColor colorWithCalibratedWhite: 0.0 alpha: 0.45]];
    [sh set];
    [rgb (90, 93, 97) set];
    [[NSBezierPath bezierPathWithOvalInRect: NSMakeRect (ctr.x - r, ctr.y - r, 2 * r, 2 * r)] fill];
    [NSGraphicsContext restoreGraphicsState];

    // Knurled skirt: alternating ridges around the rim
    const int ridges = 40;
    for (int k = 0; k < ridges; ++k)
    {
        const CGFloat a0 = (CGFloat) (2.0 * M_PI * k / ridges);
        const CGFloat a1 = (CGFloat) (2.0 * M_PI * (k + 0.5) / ridges);
        NSBezierPath* w = [NSBezierPath bezierPath];
        [w moveToPoint: ctr];
        [w lineToPoint: polar (ctr.x, ctr.y, r, a0)];
        [w lineToPoint: polar (ctr.x, ctr.y, r, a1)];
        [w closePath];
        // light from the top left
        const CGFloat lit = 0.5 + 0.5 * std::cos (a0 + 0.8);
        [[NSColor colorWithCalibratedWhite: 0.40 + 0.22 * lit alpha: 1.0] set];
        [w fill];
    }

    // Top cap
    const CGFloat rc = r * 0.78;
    NSGradient* cap = [[[NSGradient alloc] initWithStartingColor: rgb (228, 52, 46)
                                                     endingColor: rgb (150, 18, 16)] autorelease];   // red top
    NSBezierPath* capPath = [NSBezierPath bezierPathWithOvalInRect: NSMakeRect (ctr.x - rc, ctr.y - rc, 2 * rc, 2 * rc)];
    [cap drawInBezierPath: capPath angle: -60.0];
    [rgb (90, 14, 12) set];
    [capPath setLineWidth: 0.8];
    [capPath stroke];

    // White position line
    const CGFloat a = valueAngle (v);
    NSBezierPath* line = [NSBezierPath bezierPath];
    [line moveToPoint: polar (ctr.x, ctr.y, r * 0.18, a)];
    [line lineToPoint: polar (ctr.x, ctr.y, r * 0.98, a)];
    [line setLineWidth: r > 18.0 ? 3.0 : 2.4];
    [line setLineCapStyle: NSRoundLineCapStyle];
    [rgb (250, 250, 250) set];
    [line stroke];
}

// Latching push button (head select): cream cap when out, orange when pressed in (head on).
- (void) drawPush: (const Ctl&) c
{
    NSColor* ink = [NSColor blackColor];
    const bool in = isOn (shown[c.param]);
    const CGFloat h = c.r;     // half-size of the cap
    const NSRect well = NSMakeRect (c.cx - h - 5, c.cy - h - 5, 2 * h + 10, 2 * h + 10);
    NSBezierPath* wp = [NSBezierPath bezierPathWithRoundedRect: well xRadius: 4 yRadius: 4];
    [rgb (35, 36, 38) set];
    [wp fill];

    const CGFloat inset = in ? 2.5 : 0.0;
    const NSRect cap = NSMakeRect (c.cx - h + inset, c.cy - h + inset - (in ? 0.0 : 1.5), 2 * (h - inset), 2 * (h - inset));
    NSBezierPath* cp = [NSBezierPath bezierPathWithRoundedRect: cap xRadius: 3 yRadius: 3];
    [NSGraphicsContext saveGraphicsState];
    if (! in)
    {
        NSShadow* sh = [[[NSShadow alloc] init] autorelease];
        [sh setShadowOffset: NSMakeSize (0.0, -2.0)];
        [sh setShadowBlurRadius: 2.5];
        [sh setShadowColor: [NSColor colorWithCalibratedWhite: 0.0 alpha: 0.6]];
        [sh set];
    }
    NSGradient* g = in
        ? [[[NSGradient alloc] initWithStartingColor: rgb (255, 160, 60) endingColor: rgb (222, 112, 20)] autorelease]   // pressed in: orange
        : [[[NSGradient alloc] initWithStartingColor: rgb (246, 242, 230) endingColor: rgb (208, 202, 186)] autorelease];
    [g drawInBezierPath: cp angle: -90.0];
    [NSGraphicsContext restoreGraphicsState];
    [rgb (90, 88, 82) set];
    [cp setLineWidth: 0.8];
    [cp stroke];
    drawText (c.posA, boldFont (14.0), ink, NSMidX (cap), NSMidY (cap));
}

// Rocker switch: the pressed-in end is the active position.
- (void) drawRocker: (const Ctl&) c
{
    NSColor* ink = [NSColor blackColor];
    const bool on = isOn (shown[c.param]);
    const CGFloat w = 22.0, h = 36.0;
    const NSRect bezel = NSMakeRect (c.cx - w * 0.5 - 4, c.cy - h * 0.5 - 4, w + 8, h + 8);

    NSBezierPath* bz = [NSBezierPath bezierPathWithRoundedRect: bezel xRadius: 4 yRadius: 4];
    [rgb (35, 36, 38) set];
    [bz fill];

    const NSRect top = NSMakeRect (c.cx - w * 0.5, c.cy - h * 0.5, w, h * 0.5);
    const NSRect bot = NSMakeRect (c.cx - w * 0.5, c.cy, w, h * 0.5);
    // The raised half is lighter; ON = top half pressed in
    NSGradient* raised = [[[NSGradient alloc] initWithStartingColor: rgb (120, 122, 126) endingColor: rgb (66, 68, 71)] autorelease];
    NSGradient* pressed = [[[NSGradient alloc] initWithStartingColor: rgb (40, 41, 43) endingColor: rgb (58, 60, 63)] autorelease];
    [(on ? pressed : raised) drawInRect: top angle: on ? 90.0 : -90.0];
    [(on ? raised : pressed) drawInRect: bot angle: on ? 90.0 : -90.0];

    // Markings on the rocker: a bar for ON, a ring for OFF
    [[NSColor colorWithCalibratedWhite: 0.92 alpha: 1.0] set];
    NSRectFill (NSMakeRect (c.cx - 1.0, c.cy - h * 0.5 + 5, 2.0, 8.0));
    NSBezierPath* ring = [NSBezierPath bezierPathWithOvalInRect: NSMakeRect (c.cx - 4, c.cy + h * 0.5 - 13, 8, 8)];
    [ring setLineWidth: 1.6];
    [ring stroke];

    drawText (c.label, boldFont (10.0), ink, c.cx, c.cy - h * 0.5 - 14.0);
    drawTextLeft (c.posB, boldFont (9.0), ink, c.cx + w * 0.5 + 8.0, c.cy - h * 0.25);
    drawTextLeft (c.posA, boldFont (9.0), ink, c.cx + w * 0.5 + 8.0, c.cy + h * 0.25);
}

- (void) drawReadout
{
    const NSRect box = kReadoutRect;
    NSBezierPath* p = [NSBezierPath bezierPathWithRoundedRect: box xRadius: 4 yRadius: 4];
    // Fluorescent lime green, like the original unit's E.L. panel, glowing brighter at the centre
    NSGradient* glow = [[[NSGradient alloc] initWithStartingColor: rgb (206, 255, 120)
                                                      endingColor: rgb (150, 232, 70)] autorelease];
    [glow drawInBezierPath: p relativeCenterPosition: NSMakePoint (0.0, 0.0)];
    [rgb (60, 62, 65) set];
    [p setLineWidth: 1.0];
    [p stroke];

    if (status[0])
    {
        drawText (status, boldFont (14.0), [NSColor blackColor], NSMidX (box), NSMidY (box));
    }
    else if (readoutParam >= 0 && host)
    {
        char disp[16] = { 0 };
        host->getParamDisplay (readoutParam, disp);
        char text[96];
        const char* name = 0;
        for (int i = 0; i < kNumCtls; ++i)
            if (kCtls[i].param == readoutParam) { name = kCtls[i].label; break; }
        const char* unit = readoutParam == kSpeed ? " MS" : (readoutParam == kOutput ? " DB" : "");
        if (readoutParam == kWow) name = "WOW & FLUT";
        std::snprintf (text, sizeof (text), "%s   %s%s", name ? name : "", disp, unit);
        for (char* q = text; *q; ++q) if (*q >= 'a' && *q <= 'z') *q = (char) (*q - 'a' + 'A');
        drawText (text, boldFont (14.0), [NSColor blackColor], NSMidX (box), NSMidY (box));
    }
}

- (void) drawPresets
{
    NSColor* ink = [NSColor blackColor];
    drawText ("PRESETS", boldFont (10.0), ink, (NSMinX (kSaveRect) + NSMaxX (slotRect (kNumPresetSlots - 1))) * 0.5, NSMinY (kSaveRect) - 10.0);

    // SAVE button: lit red while it is waiting for a slot
    NSBezierPath* sp = [NSBezierPath bezierPathWithRoundedRect: kSaveRect xRadius: 3 yRadius: 3];
    if (saveMode)
    {
        [rgb (200, 30, 30) set];
        [sp fill];
    }
    else
    {
        NSGradient* g = [[[NSGradient alloc] initWithStartingColor: rgb (232, 234, 236) endingColor: rgb (160, 163, 167)] autorelease];
        [g drawInBezierPath: sp angle: -90.0];
    }
    [rgb (40, 41, 43) set];
    [sp setLineWidth: 1.0];
    [sp stroke];
    drawText ("SAVE", boldFont (11.0), saveMode ? [NSColor whiteColor] : ink, NSMidX (kSaveRect), NSMidY (kSaveRect));

    // Slots: empty = silver, saved = charcoal, in use = lime green
    for (int k = 0; k < kNumPresetSlots; ++k)
    {
        const NSRect r = slotRect (k);
        NSBezierPath* bp = [NSBezierPath bezierPathWithRoundedRect: r xRadius: 3 yRadius: 3];
        NSColor* fill = (k == activePreset) ? rgb (178, 245, 96) : (presetUsed[k] ? rgb (46, 47, 50) : rgb (214, 216, 219));
        NSColor* text = (k == activePreset) ? ink : (presetUsed[k] ? [NSColor whiteColor] : rgb (120, 122, 126));
        [fill set];
        [bp fill];
        [rgb (40, 41, 43) set];
        [bp setLineWidth: 1.0];
        [bp stroke];
        char num[4];
        std::snprintf (num, sizeof (num), "%d", k + 1);
        drawText (num, boldFont (12.0), text, NSMidX (r), NSMidY (r));
    }
}

// The tape loop: brown tape round a spoked reel and a capstan with its rubber
// roller, three small heads under the top run (lit orange when their button is in).
- (void) drawSpool
{
    [NSGraphicsContext saveGraphicsState];
    NSAffineTransform* xf = [NSAffineTransform transform];
    [xf translateXBy: kSpoolX yBy: kSpoolY];
    [xf scaleBy: kSpoolScale];
    [xf translateXBy: -kSpoolSrcX yBy: -kSpoolSrcY];
    [xf concat];
    [self drawSpoolUnscaled];
    [NSGraphicsContext restoreGraphicsState];
}

- (void) drawSpoolUnscaled
{
    NSColor* tapeCol = rgb (96, 56, 28);

    // Tape path: half way round the reel's left side, across, round the capstan's right side, back
    NSBezierPath* tape = [NSBezierPath bezierPath];
    [tape appendBezierPathWithArcWithCenter: NSMakePoint (kReelX, kReelY) radius: kReelR + 1.5
                                 startAngle: 270.0 endAngle: 90.0 clockwise: YES];
    [tape lineToPoint: NSMakePoint (kCapX, kCapY + kCapR + 1.5)];
    [tape appendBezierPathWithArcWithCenter: NSMakePoint (kCapX, kCapY) radius: kCapR + 1.5
                                 startAngle: 90.0 endAngle: 270.0 clockwise: YES];
    [tape closePath];

    // Heads under the top run of tape
    const CGFloat headX[3] = { 625.0, 643.0, 661.0 };
    for (int i = 0; i < 3; ++i)
    {
        const CGFloat t = (headX[i] - kReelX) / (kCapX - kReelX);
        const CGFloat runY = (kReelY - kReelR - 1.5) + t * ((kCapY - kCapR - 1.5) - (kReelY - kReelR - 1.5));
        const NSRect hr = NSMakeRect (headX[i] - 5.0, runY + 1.0, 10.0, 9.0);
        NSBezierPath* hp = [NSBezierPath bezierPathWithRoundedRect: hr xRadius: 1.5 yRadius: 1.5];
        const bool on = isOn (shown[kHead1 + i]);
        [(on ? rgb (240, 130, 30) : rgb (72, 74, 78)) set];
        [hp fill];
        [rgb (30, 31, 33) set];
        [hp setLineWidth: 0.6];
        [hp stroke];
    }

    [tapeCol set];
    [tape setLineWidth: 3.0];
    [tape stroke];
    // Moving texture on the tape so it visibly runs
    NSBezierPath* tex = [[tape copy] autorelease];
    const CGFloat dash[2] = { 3.0, 6.0 };
    [tex setLineDash: dash count: 2 phase: (CGFloat) -std::fmod (tapePx, 9.0)];
    [rgb (150, 98, 58) set];
    [tex setLineWidth: 1.4];
    [tex stroke];

    // Reel: silver disc, dark rim, three spokes turning with the tape
    NSBezierPath* disc = [NSBezierPath bezierPathWithOvalInRect: NSMakeRect (kReelX - kReelR, kReelY - kReelR, 2 * kReelR, 2 * kReelR)];
    NSGradient* g = [[[NSGradient alloc] initWithStartingColor: rgb (226, 228, 231) endingColor: rgb (150, 153, 158)] autorelease];
    [g drawInBezierPath: disc angle: -60.0];
    [rgb (55, 57, 60) set];
    [disc setLineWidth: 1.0];
    [disc stroke];
    const CGFloat reelA = (CGFloat) (tapePx / kReelR);
    for (int k = 0; k < 3; ++k)
    {
        const CGFloat a = reelA + (CGFloat) (k * 2.0 * M_PI / 3.0);
        NSBezierPath* sp = [NSBezierPath bezierPath];
        [sp moveToPoint: polar (kReelX, kReelY, 4.0, a)];
        [sp lineToPoint: polar (kReelX, kReelY, kReelR - 3.0, a)];
        [sp setLineWidth: 2.2];
        [sp setLineCapStyle: NSRoundLineCapStyle];
        [rgb (80, 82, 86) set];
        [sp stroke];
    }
    [rgb (40, 41, 43) set];
    [[NSBezierPath bezierPathWithOvalInRect: NSMakeRect (kReelX - 3.5, kReelY - 3.5, 7.0, 7.0)] fill];

    // Capstan: black rubber roller with a silver shaft and a turning mark
    [rgb (28, 28, 30) set];
    [[NSBezierPath bezierPathWithOvalInRect: NSMakeRect (kCapX - kCapR, kCapY - kCapR, 2 * kCapR, 2 * kCapR)] fill];
    [rgb (190, 193, 197) set];
    [[NSBezierPath bezierPathWithOvalInRect: NSMakeRect (kCapX - 2.5, kCapY - 2.5, 5.0, 5.0)] fill];
    const NSPoint m = polar (kCapX, kCapY, kCapR - 2.5, (CGFloat) (tapePx / kCapR));
    [rgb (120, 122, 126) set];
    [[NSBezierPath bezierPathWithOvalInRect: NSMakeRect (m.x - 1.3, m.y - 1.3, 2.6, 2.6)] fill];
}

- (void) drawRect: (NSRect) dirty
{
    // Animation frames only touch the tape loop: redraw just that corner
    if (NSContainsRect (kSpoolRect, dirty))
    {
        [self drawBackground];
        [self drawSpool];
        return;
    }
    [self drawBackground];
    [self drawSpool];
    NSColor* ink = [NSColor blackColor];

    for (int i = 0; i < kNumCtls; ++i)
    {
        const Ctl& c = kCtls[i];
        switch (c.type)
        {
            case kCtlKnob:
                [self drawScaleForCtl: c];
                [self drawKnobAt: NSMakePoint (c.cx, c.cy) radius: c.r value: shown[c.param]];
                drawText (c.label, boldFont (11.0), ink, c.cx, c.cy - c.r - 34.0);
                break;
            case kCtlPush:
                [self drawPush: c];
                break;
            case kCtlRocker:
                [self drawRocker: c];
                break;
            case kCtlSmallKnob:
            {
                [self drawKnobAt: NSMakePoint (c.cx, c.cy) radius: c.r value: shown[c.param]];
                drawText (c.label, boldFont (10.0), ink, c.cx, c.cy - c.r - 14.0);
                drawText (c.posA, plainFont (8.5), ink, c.cx - c.r - 16.0, c.cy + c.r - 2.0);
                drawText (c.posB, plainFont (8.5), ink, c.cx + c.r + 16.0, c.cy + c.r - 2.0);
                break;
            }
            default: break;
        }
    }
    [self drawPresets];
    [self drawReadout];
}

//------------------------------------------------------------------------------
// Mouse

- (int) ctlAtPoint: (NSPoint) p
{
    for (int i = 0; i < kNumCtls; ++i)
    {
        const Ctl& c = kCtls[i];
        if (c.type == kCtlRocker)
        {
            if (std::fabs (p.x - c.cx) < 20 && std::fabs (p.y - c.cy) < 26) return i;
        }
        else if (c.type == kCtlPush)
        {
            if (std::fabs (p.x - c.cx) < c.r + 6 && std::fabs (p.y - c.cy) < c.r + 6) return i;
        }
        else
        {
            const CGFloat dx = p.x - c.cx, dy = p.y - c.cy;
            if (dx * dx + dy * dy < (c.r + 8) * (c.r + 8)) return i;
        }
    }
    return -1;
}

- (void) setParam: (int) param value: (float) v
{
    if (! host) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    host->guiSetParam (param, v);
    shown[param] = host->getParam (param);
    readoutParam = param;
    [self setNeedsDisplay: YES];
}

// SAVE button and preset slots. Returns YES if the click was handled here.
- (BOOL) presetClick: (NSPoint) p
{
    if (NSPointInRect (p, kSaveRect))
    {
        saveMode = ! saveMode;
        std::snprintf (status, sizeof (status), "%s", saveMode ? "SAVE: CLICK A SLOT 1-8" : "");
        [self setNeedsDisplay: YES];
        return YES;
    }
    for (int k = 0; k < kNumPresetSlots; ++k)
    {
        if (! NSPointInRect (p, slotRect (k))) continue;
        if (saveMode)
        {
            const bool ok = host->savePreset (k);
            saveMode = false;
            host->refreshPresets();
            for (int j = 0; j < kNumPresetSlots; ++j) presetUsed[j] = host->presetExists (j);
            if (ok) activePreset = k;
            std::snprintf (status, sizeof (status), ok ? "PRESET %d SAVED" : "COULD NOT SAVE PRESET %d", k + 1);
        }
        else
        {
            host->refreshPresets();
            for (int j = 0; j < kNumPresetSlots; ++j) presetUsed[j] = host->presetExists (j);
            if (presetUsed[k] && host->loadPreset (k))
            {
                for (int j = 0; j < kNumParams; ++j) shown[j] = host->getParam (j);
                activePreset = k;
                std::snprintf (status, sizeof (status), "PRESET %d LOADED", k + 1);
            }
            else
                std::snprintf (status, sizeof (status), "PRESET %d IS EMPTY", k + 1);
        }
        [self setNeedsDisplay: YES];
        return YES;
    }
    return NO;
}

- (void) mouseDown: (NSEvent*) e
{
    const NSPoint p = [self convertPoint: [e locationInWindow] fromView: nil];
    if (! host) return;
    if ([self presetClick: p]) return;
    const int i = [self ctlAtPoint: p];
    if (i < 0) return;
    const Ctl& c = kCtls[i];
    readoutParam = c.param;
    status[0] = 0;
    activePreset = -1;     // settings are being changed away from the preset

    switch (c.type)
    {
        case kCtlRocker:
        {
            // Press the top half for ON, the bottom half for OFF
            host->guiBeginEdit (c.param);
            [self setParam: c.param value: (p.y < c.cy) ? 1.0f : 0.0f];
            host->guiEndEdit (c.param);
            return;
        }
        case kCtlPush:
        {
            host->guiBeginEdit (c.param);
            [self setParam: c.param value: isOn (shown[c.param]) ? 0.0f : 1.0f];
            host->guiEndEdit (c.param);
            return;
        }
        default:
            break;
    }

    host->guiBeginEdit (c.param);
    if ([e clickCount] == 2)
        [self setParam: c.param value: kDefaults[c.param]];   // double-click: back to default
    dragCtl = i;
    dragStartY = p.y;
    dragStartValue = shown[c.param];
    [self setNeedsDisplay: YES];
}

- (void) mouseDragged: (NSEvent*) e
{
    if (dragCtl < 0 || ! host) return;
    const Ctl& c = kCtls[dragCtl];
    const NSPoint p = [self convertPoint: [e locationInWindow] fromView: nil];
    const bool fine = ([e modifierFlags] & NSShiftKeyMask) != 0;
    const CGFloat perPixel = fine ? 1.0 / 1000.0 : 1.0 / 200.0;
    const CGFloat v = dragStartValue + (dragStartY - p.y) * perPixel;
    [self setParam: c.param value: (float) v];
}

- (void) mouseUp: (NSEvent*) e
{
    (void) e;
    if (dragCtl >= 0 && host) host->guiEndEdit (kCtls[dragCtl].param);
    dragCtl = -1;
}

- (void) scrollWheel: (NSEvent*) e
{
    const NSPoint p = [self convertPoint: [e locationInWindow] fromView: nil];
    const int i = [self ctlAtPoint: p];
    if (i < 0 || ! host) return;
    const Ctl& c = kCtls[i];
    if (c.type != kCtlKnob && c.type != kCtlSmallKnob) return;
    const CGFloat d = [e deltaY];
    if (d == 0.0) return;
    status[0] = 0;
    activePreset = -1;
    host->guiBeginEdit (c.param);
    [self setParam: c.param value: (float) (shown[c.param] + d * 0.01)];
    host->guiEndEdit (c.param);
}

@end

//==============================================================================
namespace tbcc
{

// Both calls use their own autorelease pool, so nothing of this plugin's is
// left waiting in the host's pool when the editor closes or the plugin unloads.
void* editorOpen (void* parentView, EditorHost* host)
{
    NSView* parent = (NSView*) parentView;
    if (! parent || ! host) return 0;
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    TBCopicatPanelView* v = [[TBCopicatPanelView alloc]
                                  initWithFrame: NSMakeRect (0, 0, kEditorWidth, kEditorHeight) host: host];
    if (v) [parent addSubview: v];
    [pool release];
    return (void*) v;    // retained until editorClose
}

void editorClose (void* handle)
{
    TBCopicatPanelView* v = (TBCopicatPanelView*) handle;
    if (! v) return;
    NSAutoreleasePool* pool = [[NSAutoreleasePool alloc] init];
    [v shutdown];
    [v removeFromSuperview];
    [v release];
    [pool release];
}

} // namespace tbcc
