/* Hope UI Engine - css parsing, selectors and the cascade */
#ifndef HPE_CSS_H
#define HPE_CSS_H

#include "base/hpe_types.h"
#include "base/hpe_util.h"
#include "dom/hpe_dom.h"

typedef enum HpeDisplay {
    HPE_DISPLAY_INLINE = 0,
    HPE_DISPLAY_BLOCK,
    HPE_DISPLAY_NONE,
    HPE_DISPLAY_INLINE_BLOCK,
    HPE_DISPLAY_FLEX
} HpeDisplay;

/* flex container axes; only `row` is reverse-free, `*-reverse` is unsupported */
typedef enum HpeFlexDir {
    HPE_FLEX_ROW = 0,
    HPE_FLEX_COLUMN
} HpeFlexDir;

/* align-items / align-self; HPE_ALIGN_ITEMS_AUTO only appears in align-self */
typedef enum HpeAlignItems {
    HPE_ALIGN_ITEMS_STRETCH = 0,
    HPE_ALIGN_ITEMS_FLEX_START,
    HPE_ALIGN_ITEMS_CENTER,
    HPE_ALIGN_ITEMS_FLEX_END,
    HPE_ALIGN_ITEMS_BASELINE,
    HPE_ALIGN_ITEMS_AUTO
} HpeAlignItems;

typedef enum HpeJustify {
    HPE_JUSTIFY_START = 0,
    HPE_JUSTIFY_CENTER,
    HPE_JUSTIFY_END,
    HPE_JUSTIFY_SPACE_BETWEEN,
    HPE_JUSTIFY_SPACE_AROUND
} HpeJustify;


typedef enum HpePosition {
    HPE_POS_STATIC = 0,
    HPE_POS_RELATIVE,
    HPE_POS_ABSOLUTE,
    HPE_POS_FIXED
} HpePosition;

typedef enum HpeFloatSide {
    HPE_FLOAT_NONE = 0,
    HPE_FLOAT_LEFT,
    HPE_FLOAT_RIGHT
} HpeFloatSide;

typedef enum HpeClearSide {
    HPE_CLEAR_NONE = 0,
    HPE_CLEAR_LEFT,
    HPE_CLEAR_RIGHT,
    HPE_CLEAR_BOTH
} HpeClearSide;

typedef enum HpeTextAlign {
    HPE_ALIGN_LEFT = 0,
    HPE_ALIGN_CENTER,
    HPE_ALIGN_RIGHT
} HpeTextAlign;

typedef enum HpeWhiteSpace {
    HPE_WS_NORMAL = 0,
    HPE_WS_NOWRAP,
    HPE_WS_PRE,
    HPE_WS_PRE_WRAP,
    HPE_WS_PRE_LINE
} HpeWhiteSpace;

typedef enum HpeBoxSizing {
    HPE_BOX_SIZING_CONTENT = 0,
    HPE_BOX_SIZING_BORDER
} HpeBoxSizing;

typedef enum HpeOverflow {
    HPE_OVERFLOW_VISIBLE = 0,
    HPE_OVERFLOW_HIDDEN,
    HPE_OVERFLOW_SCROLL,
    HPE_OVERFLOW_AUTO
} HpeOverflow;

typedef enum HpeVisibility {
    HPE_VISIBILITY_VISIBLE = 0,
    HPE_VISIBILITY_HIDDEN,
    HPE_VISIBILITY_COLLAPSE
} HpeVisibility;

typedef enum HpeTextDecoration {
    HPE_TD_NONE = 0,
    HPE_TD_UNDERLINE = 1 << 0,
    HPE_TD_OVERLINE = 1 << 1,
    HPE_TD_LINE_THROUGH = 1 << 2
} HpeTextDecoration;

typedef enum HpeListStyleType {
    HPE_LIST_DISC = 0,
    HPE_LIST_CIRCLE,
    HPE_LIST_SQUARE,
    HPE_LIST_DECIMAL,
    HPE_LIST_NONE
} HpeListStyleType;

typedef enum HpeListStylePosition {
    HPE_LIST_OUTSIDE = 0,
    HPE_LIST_INSIDE
} HpeListStylePosition;

typedef enum HpeVerticalAlign {
    HPE_VALIGN_BASELINE = 0,
    HPE_VALIGN_MIDDLE,
    HPE_VALIGN_TOP,
    HPE_VALIGN_BOTTOM,
    HPE_VALIGN_TEXT_TOP,
    HPE_VALIGN_TEXT_BOTTOM
} HpeVerticalAlign;

/* one stop of a linear gradient */
typedef struct HpeGradientStop {
    float pos; /* 0..1 */
    HpeColor color;
} HpeGradientStop;

typedef enum HpeBackgroundKind {
    HPE_BG_NONE = 0,
    HPE_BG_LINEAR_GRADIENT
} HpeBackgroundKind;

/* parsed `background-image`: a linear gradient plus its stored stops */
#define HPE_MAX_GRADIENT_STOPS 8
typedef struct HpeBackground {
    HpeBackgroundKind kind;
    float angle_deg; /* 0 = to top, 90 = to right (css `to` direction mapped to degrees) */
    HpeGradientStop stops[HPE_MAX_GRADIENT_STOPS];
    int stop_count;
} HpeBackground;

/* a box-shadow / text-shadow (one layer) */
typedef struct HpeShadow {
    float ox, oy, blur, spread;
    HpeColor color;
    int valid;
} HpeShadow;

/* a 2d affine transform: [a c e; b d f; 0 0 1] */
typedef struct HpeTransform {
    float a, b, c, d, e, f;
    int identity;
} HpeTransform;

/* parsed `cursor` value; the Win32 chrome maps these to system cursor ids */
typedef enum HpeCursor {
    HPE_CURSOR_AUTO = 0,
    HPE_CURSOR_DEFAULT,
    HPE_CURSOR_POINTER,
    HPE_CURSOR_TEXT,
    HPE_CURSOR_MOVE,
    HPE_CURSOR_WAIT,
    HPE_CURSOR_HELP,
    HPE_CURSOR_CROSSHAIR,
    HPE_CURSOR_NOT_ALLOWED,
    HPE_CURSOR_GRAB,
    HPE_CURSOR_GRABBING,
    HPE_CURSOR_EW_RESIZE,
    HPE_CURSOR_NS_RESIZE
} HpeCursor;

/* relationship of a compound to the compound on its left inside a selector */
typedef enum HpeCombinator {
    HPE_COMB_DESCENDANT = 0, /* (space) */
    HPE_COMB_CHILD,          /* > */
    HPE_COMB_ADJ_SIBLING,    /* + */
    HPE_COMB_GEN_SIBLING     /* ~ */
} HpeCombinator;

/* one bit per longhand; the style struct carries the resolved values */
#define HPE_PROP_DISPLAY       (1ull << 0)
#define HPE_PROP_COLOR         (1ull << 1)
#define HPE_PROP_BACKGROUND    (1ull << 2)
#define HPE_PROP_WIDTH         (1ull << 3)
#define HPE_PROP_HEIGHT        (1ull << 4)
#define HPE_PROP_MARGIN_TOP    (1ull << 5)
#define HPE_PROP_MARGIN_RIGHT  (1ull << 6)
#define HPE_PROP_MARGIN_BOTTOM (1ull << 7)
#define HPE_PROP_MARGIN_LEFT   (1ull << 8)
#define HPE_PROP_PADDING_TOP   (1ull << 9)
#define HPE_PROP_PADDING_RIGHT (1ull << 10)
#define HPE_PROP_PADDING_BOTTOM (1ull << 11)
#define HPE_PROP_PADDING_LEFT  (1ull << 12)
#define HPE_PROP_BORDER_WIDTH  (1ull << 13)
#define HPE_PROP_BORDER_COLOR  (1ull << 14)
#define HPE_PROP_FONT_SIZE     (1ull << 15)
#define HPE_PROP_FONT_WEIGHT   (1ull << 16)
#define HPE_PROP_TEXT_ALIGN    (1ull << 17)
#define HPE_PROP_LINE_HEIGHT   (1ull << 18)
#define HPE_PROP_WHITE_SPACE   (1ull << 19)
#define HPE_PROP_FONT_FAMILY   (1ull << 20)
#define HPE_PROP_POSITION      (1ull << 21)
#define HPE_PROP_INSET_TOP     (1ull << 22)
#define HPE_PROP_INSET_RIGHT   (1ull << 23)
#define HPE_PROP_INSET_BOTTOM  (1ull << 24)
#define HPE_PROP_INSET_LEFT    (1ull << 25)
#define HPE_PROP_Z_INDEX       (1ull << 26)
#define HPE_PROP_FLOAT         (1ull << 27)
#define HPE_PROP_CLEAR         (1ull << 28)
#define HPE_PROP_BORDER_RADIUS (1ull << 29)
/* per-side border widths follow the HPE_PROP_BORDER_TOP + i pattern (t, r, b, l) */
#define HPE_PROP_BORDER_TOP    (1ull << 30)
#define HPE_PROP_BORDER_RIGHT  (1ull << 31)
#define HPE_PROP_BORDER_BOTTOM (1ull << 32)
#define HPE_PROP_BORDER_LEFT   (1ull << 33)
#define HPE_PROP_FLEX_DIR      (1ull << 34)
#define HPE_PROP_ALIGN_ITEMS   (1ull << 35)
#define HPE_PROP_JUSTIFY       (1ull << 36)
#define HPE_PROP_GAP           (1ull << 37)
#define HPE_PROP_FLEX_GROW     (1ull << 38)
#define HPE_PROP_ALIGN_SELF    (1ull << 39)
#define HPE_PROP_MIN_WIDTH     (1ull << 40)

/* A second word is needed because the property set outgrew 64 bits; properties
 * from here on use HPE_PROP2_* and are applied from `HpeStyle.set2`. */
#define HPE_PROP2_BORDER_TOP_COLOR    (1ull << 0)
#define HPE_PROP2_BORDER_RIGHT_COLOR  (1ull << 1)
#define HPE_PROP2_BORDER_BOTTOM_COLOR (1ull << 2)
#define HPE_PROP2_BORDER_LEFT_COLOR   (1ull << 3)
#define HPE_PROP2_MIN_HEIGHT          (1ull << 4)
#define HPE_PROP2_MAX_WIDTH           (1ull << 5)
#define HPE_PROP2_MAX_HEIGHT          (1ull << 6)
#define HPE_PROP2_BOX_SIZING          (1ull << 7)
#define HPE_PROP2_OVERFLOW_X          (1ull << 8)
#define HPE_PROP2_OVERFLOW_Y          (1ull << 9)
#define HPE_PROP2_OPACITY             (1ull << 10)
#define HPE_PROP2_VISIBILITY          (1ull << 11)
#define HPE_PROP2_CURSOR              (1ull << 12)
#define HPE_PROP2_TEXT_DECORATION     (1ull << 13)
#define HPE_PROP2_LETTER_SPACING      (1ull << 14)
#define HPE_PROP2_WORD_SPACING        (1ull << 15)
#define HPE_PROP2_TEXT_INDENT         (1ull << 16)
#define HPE_PROP2_VERTICAL_ALIGN      (1ull << 17)
#define HPE_PROP2_LIST_STYLE_TYPE     (1ull << 18)
#define HPE_PROP2_LIST_STYLE_POSITION (1ull << 19)
#define HPE_PROP2_BACKGROUND_IMAGE    (1ull << 20)
#define HPE_PROP2_BOX_SHADOW          (1ull << 21)
#define HPE_PROP2_TEXT_SHADOW         (1ull << 22)
#define HPE_PROP2_TRANSFORM           (1ull << 23)
#define HPE_PROP2_TRANSITION          (1ull << 24)
#define HPE_PROP2_ANIMATION           (1ull << 25)
/* per-side border color uses one bit plus the shared HpeStyle.border_colors[4] */

/* A fully resolved (or partially, via `set`) inline style.
 * `line_height` <= 0 means "auto", a negative value -n means the n-multiple of font-size.
 * `border_w[]` is ordered top, right, bottom, left — the same order as margin/padding.
 * `imp` / `imp2` mirror `set` / `set2` and mark which declared properties carried
 * `!important`; the cascade applies normal declarations first, then important ones. */
typedef struct HpeStyle {
    uint64_t set;
    uint64_t set2; /* second property word, see HPE_PROP2_* */
    uint64_t imp;  /* important bits for `set` */
    uint64_t imp2; /* important bits for `set2` */
    HpeDisplay display;
    HpeColor color;
    HpeColor background;
    HpeColor border_color;            /* shorthand colour for all four sides */
    HpeColor border_colors[4];        /* per-side, top right bottom left */
    HpeLength width;
    HpeLength height;
    HpeLength margin[4];  /* top right bottom left */
    HpeLength padding[4]; /* top right bottom left */
    float border_w[4];    /* top right bottom left */
    float border_radius;
    float font_size;
    int font_weight;
    HpeTextAlign text_align;
    float line_height;
    HpeWhiteSpace white_space;
    HpeFontFamily family;
    HpePosition position;
    HpeLength inset[4];   /* top, right, bottom, left */
    int z_index;
    int z_auto;
    HpeFloatSide float_side;
    HpeClearSide clear_side;
    int flex_dir;
    int align_items;
    int justify;
    int align_self;  /* HPE_ALIGN_ITEMS_AUTO when unset */
    float gap;
    float flex_grow;
    HpeLength min_width;
    HpeLength min_height;
    HpeLength max_width;
    HpeLength max_height;
    HpeBoxSizing box_sizing;
    HpeOverflow overflow_x;
    HpeOverflow overflow_y;
    float opacity;
    HpeVisibility visibility;
    int cursor; /* HpeCursor id, parsed but not yet applied by the chrome */
    HpeTextDecoration text_decoration;
    HpeLength letter_spacing; /* may be negative; `auto` means `normal` (0) */
    HpeLength word_spacing;
    HpeLength text_indent;
    HpeVerticalAlign vertical_align;
    HpeListStyleType list_style_type;
    HpeListStylePosition list_style_position;
    HpeBackground background_image;
    HpeShadow box_shadow;
    HpeShadow text_shadow;
    HpeTransform transform;
    int has_transition; /* parsed presence only; no interpolation clock yet */
    int has_animation;
} HpeStyle;

typedef struct HpeAttrSel {
    char *name;
    char *op;   /* "", "=", "~=", "|=", "^=", "$=", "*=" */
    char *value;
} HpeAttrSel;

/* ---------------- media queries ---------------- */

/* recognised media features (the `(...)` conditions in a query) */
#define HPE_MF_MIN_WIDTH  1
#define HPE_MF_MAX_WIDTH  2
#define HPE_MF_MIN_HEIGHT 3
#define HPE_MF_MAX_HEIGHT 4
#define HPE_MF_WIDTH      5
#define HPE_MF_HEIGHT     6

/* media types the engine distinguishes; anything else is treated as `all` */
#define HPE_MTYPE_UNSET  -1
#define HPE_MTYPE_ALL    0
#define HPE_MTYPE_SCREEN 1
#define HPE_MTYPE_PRINT  2

#define HPE_MEDIA_MAX_ALT   4 /* comma-separated alternatives (OR) */
#define HPE_MEDIA_MAX_COND  4 /* `and`-joined conditions inside one alternative */
#define HPE_MEDIA_MAX_DEPTH 2 /* nested @media blocks */

/* one comma-separated alternative of an @media condition list */
typedef struct HpeMediaAlt {
    int negate;    /* leading `not` */
    int type;      /* HPE_MTYPE_* */
    int unknown;   /* an unrecognised type/feature forces a mismatch */
    int cond_count;
    struct {
        int feature; /* HPE_MF_* */
        float value; /* px */
    } conds[HPE_MEDIA_MAX_COND];
} HpeMediaAlt;

/* one @media condition list, evaluated against the viewport at cascade time
 * (so a resize re-evaluates correctly without re-parsing the stylesheet). */
typedef struct HpeMediaQuery {
    int present; /* 0 = the rule is unconditional */
    int alt_count;
    HpeMediaAlt alts[HPE_MEDIA_MAX_ALT];
} HpeMediaQuery;

typedef struct HpeCompound {
    char *tag; /* NULL or "*" means any element */
    char *id;
    char **classes;
    int class_count;
    size_t class_cap;
    HpeAttrSel *attrs;
    int attr_count;
    size_t attr_cap;
    int pseudo_hover; /* :hover — matches only the hovered element */
    int comb; /* HpeCombinator for this part (relative to the previous part) */
} HpeCompound;

typedef struct HpeSelector {
    HpeCompound *parts; /* left to right, the last part is the subject */
    int count;
    size_t cap;
    int specificity;
} HpeSelector;

typedef struct HpeRule {
    HpeSelector *selectors;
    int selector_count;
    size_t selector_cap;
    HpeStyle *decls;
    int decl_count;
    size_t decl_cap;
    int order;
    /* conditions from enclosing @media blocks (all must match); 0 = unconditional */
    HpeMediaQuery mq[HPE_MEDIA_MAX_DEPTH];
    int mq_depth;
} HpeRule;

typedef struct HpeStylesheet {
    HpeRule *rules;
    int rule_count;
    size_t rule_cap;
    int next_order;
    float vw; /* viewport width/height used to evaluate @media at cascade time */
    float vh;
} HpeStylesheet;

void hpe_css_init(HpeStylesheet *sheet);
void hpe_css_free(HpeStylesheet *sheet);
void hpe_css_parse(HpeStylesheet *sheet, const char *css, size_t length);
/* sets the viewport used to evaluate @media queries (call before a re-cascade) */
void hpe_css_set_viewport(HpeStylesheet *sheet, float vw, float vh);

/* parses a standalone selector (possibly comma-separated) and tests it against
 * `node`; combinators walk node's ancestors/siblings. used by querySelector. */
int hpe_css_selector_matches(const HpeDomNode *node, const char *sel, size_t len);

HpeStyle hpe_style_default(void);
/* parses a declaration block (the content of a style="" attribute) */
void hpe_css_parse_declarations(const char *text, size_t length, HpeStyle *decl);
/* computes the style of `node`, inheriting from `parent` (may be NULL) */
void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent,
                     HpeStyle *out);

const char *hpe_ua_stylesheet(void);

/* inline style="..." helpers used by the javascript DOM bridge.
 * `prop` must be the css property name (lowercase, dashed). */
void hpe_css_inline_set(HpeDomNode *node, const char *prop, const char *value);
int hpe_css_inline_get(const HpeDomNode *node, const char *prop, char *out, size_t out_size);

#endif /* HPE_CSS_H */
