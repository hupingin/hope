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
    HPE_DISPLAY_FLEX,
    HPE_DISPLAY_GRID,            /* grid container */
    HPE_DISPLAY_TABLE,           /* the table wrapper/root */
    HPE_DISPLAY_TABLE_ROW,       /* <tr> */
    HPE_DISPLAY_TABLE_CELL,      /* <td> / <th> */
    HPE_DISPLAY_TABLE_ROW_GROUP, /* <thead> / <tbody> / <tfoot> */
    HPE_DISPLAY_TABLE_CAPTION
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
    HPE_BG_LINEAR_GRADIENT,
    HPE_BG_URL /* an external image referenced by `background-image: url(...)` */
} HpeBackgroundKind;

/* how a replaced element's content is fitted into its content box */
typedef enum HpeObjectFit {
    HPE_OBJECT_FIT_FILL = 0,
    HPE_OBJECT_FIT_CONTAIN,
    HPE_OBJECT_FIT_COVER,
    HPE_OBJECT_FIT_NONE,
    HPE_OBJECT_FIT_SCALE_DOWN
} HpeObjectFit;

/* how a background image layer tiles inside its positioning area */
typedef enum HpeBgRepeat {
    HPE_BG_REPEAT_REPEAT = 0,
    HPE_BG_REPEAT_X,
    HPE_BG_REPEAT_Y,
    HPE_BG_REPEAT_NO_REPEAT
} HpeBgRepeat;

/* longest storable `url()` reference in `background-image` */
#define HPE_BG_URL_MAX 512

/* parsed `background-image`: a linear gradient plus its stored stops, or an
 * external image referenced by `url(...)` */
#define HPE_MAX_GRADIENT_STOPS 8
typedef struct HpeBackground {
    HpeBackgroundKind kind;
    float angle_deg; /* 0 = to top, 90 = to right (css `to` direction mapped to degrees) */
    HpeGradientStop stops[HPE_MAX_GRADIENT_STOPS];
    int stop_count;
    char url[HPE_BG_URL_MAX]; /* `kind == HPE_BG_URL`: the raw `url()` argument */
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

/* flattening limits shared by the style struct and the cascade */
#define HPE_CONTENT_MAX 256
#define HPE_GRID_MAX_TRACKS 12

/* a `background-position` / `background-size` pair (px or %; `auto` = negative sentinel) */
typedef struct HpeBgPos {
    float x, y;      /* position: px, or a percentage when `x_pct`/`y_pct` */
    int x_pct, y_pct;
    float size_x, size_y; /* size: px, or a percentage when `sx_pct`/`sy_pct` */
    int sx_pct, sy_pct;
    int size_auto;        /* 1 = `background-size: auto` (use the intrinsic size) */
    int size_cover, size_contain;
    int present;
} HpeBgPos;

/* one `grid-template-*` track: a length or a fraction (`fr`) */
typedef struct HpeGridTrack {
    int kind; /* 0 = px, 1 = fr, 2 = auto, 3 = % */
    float value;
} HpeGridTrack;

typedef struct HpeGridSpec {
    HpeGridTrack tracks[HPE_GRID_MAX_TRACKS];
    int count;
    int present;
} HpeGridSpec;

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
#define HPE_PROP2_CONTENT             (1ull << 26)
#define HPE_PROP2_FLEX_WRAP           (1ull << 27)
#define HPE_PROP2_ORDER               (1ull << 28)
#define HPE_PROP2_FLEX_BASIS          (1ull << 29)
#define HPE_PROP2_FLEX_SHRINK         (1ull << 30)
#define HPE_PROP2_BACKGROUND_POSITION (1ull << 31)
#define HPE_PROP2_BACKGROUND_SIZE     (1ull << 32)
#define HPE_PROP2_RADIAL_GRADIENT     (1ull << 33)
#define HPE_PROP2_GRID_TEMPLATE_COLUMNS (1ull << 34)
#define HPE_PROP2_GRID_TEMPLATE_ROWS  (1ull << 35)
#define HPE_PROP2_GRID_GAP            (1ull << 36)
#define HPE_PROP2_DIRECTION           (1ull << 37)
#define HPE_PROP2_SCROLLBAR_WIDTH     (1ull << 38)
#define HPE_PROP2_TABLE_LAYOUT        (1ull << 39)
#define HPE_PROP2_BORDER_SPACING      (1ull << 40)
#define HPE_PROP2_ROW_REVERSE         (1ull << 41)
#define HPE_PROP2_GRID_COLUMN         (1ull << 42)
#define HPE_PROP2_GRID_ROW            (1ull << 43)
#define HPE_PROP2_JUSTIFY_ITEMS       (1ull << 44)
#define HPE_PROP2_ALIGN_CONTENT       (1ull << 45)
#define HPE_PROP2_GRID_AUTO_FLOW      (1ull << 46)
/* geometric properties of an external image layer: `background-repeat` plus the
 * replaced-element `object-fit` / `object-position` pair */
#define HPE_PROP2_BACKGROUND_REPEAT   (1ull << 47)
#define HPE_PROP2_OBJECT_FIT          (1ull << 48)
#define HPE_PROP2_OBJECT_POSITION     (1ull << 49)
/* per-side border color uses one bit plus the shared HpeStyle.border_colors[4] */

/* `scrollbar-width` initial value is `auto`, i.e. a classic 12px scrollbar. The
 * resolved width is stored as plain pixels, and 0 (`scrollbar-width: none`) is a
 * legal value, so the default cannot double as a "not set" sentinel. */
#define HPE_SCROLLBAR_WIDTH_AUTO 12.0f

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
    /* generated content, resolved to text for ::before / ::after */
    char content[HPE_CONTENT_MAX];
    char attr_content[96]; /* `attr(name)` argument of `content`, resolved at box build */
    int has_content;
    /* flex item longhands */
    int flex_wrap;         /* 0 = nowrap, 1 = wrap, 2 = wrap-reverse */
    int order;
    HpeLength flex_basis;
    float flex_shrink;
    int row_reverse;       /* flex-direction: *-reverse */
    /* background placement (background-position / background-size) */
    HpeBgPos bg_pos;
    /* radial gradient parameters (kind stays HPE_BG_LINEAR_GRADIENT for linear) */
    int bg_radial;         /* 1 when `background-image` was a radial-gradient */
    int bg_radial_circle;  /* 1 for `circle`, 0 for `ellipse` */
    float bg_radial_cx, bg_radial_cy, bg_radial_rx, bg_radial_ry; /* 0..1 relative */
    /* grid container specs */
    HpeGridSpec grid_cols;
    HpeGridSpec grid_rows;
    float grid_gap;
    /* grid item placement (`grid-column` / `grid-row`, 1-based; 0 = auto,
     * a negative value -n means `span n`) */
    int grid_col_start, grid_col_end;
    int grid_row_start, grid_row_end;
    int justify_items;   /* HpeAlignItems values, applied to grid items */
    int align_content;   /* HpeJustify values, applied to the whole grid */
    int grid_auto_flow;  /* 0 = row, 1 = column */
    int table_layout_fixed;
    float border_spacing_h, border_spacing_v;
    int direction_rtl;
    float scrollbar_width;
    /* replaced-element content fitting and external background-image tiling */
    HpeObjectFit object_fit;
    HpeBgPos object_pos; /* `object-position`, shares HpeBgPos' px / % form */
    HpeBgRepeat bg_repeat;
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

/* pseudo-elements the engine materialises as anonymous boxes */
typedef enum HpePseudoElement {
    HPE_PE_NONE = 0,
    HPE_PE_BEFORE,
    HPE_PE_AFTER
} HpePseudoElement;

/* state / structural pseudo-class bits; a compound selector may carry several
 * (`a:hover.foo:first-child`). Parametric forms (`:nth-child(an+b)`, `:not()`,
 * `:is()`, `:where()`) have dedicated fields below instead. */
#define HPE_PS_HOVER        (1u << 0)
#define HPE_PS_FOCUS        (1u << 1)
#define HPE_PS_ACTIVE       (1u << 2)
#define HPE_PS_VISITED      (1u << 3)
#define HPE_PS_FIRST_CHILD  (1u << 4)
#define HPE_PS_LAST_CHILD   (1u << 5)
#define HPE_PS_ONLY_CHILD   (1u << 6)
#define HPE_PS_FIRST_OF_TYPE (1u << 7)
#define HPE_PS_LAST_OF_TYPE  (1u << 8)
#define HPE_PS_ONLY_OF_TYPE  (1u << 9)
#define HPE_PS_EMPTY         (1u << 10)
#define HPE_PS_CHECKED       (1u << 11)
#define HPE_PS_DISABLED      (1u << 12)
#define HPE_PS_ENABLED       (1u << 13)
#define HPE_PS_LINK          (1u << 14)
#define HPE_PS_TARGET        (1u << 15)

/* one `an+b` formula as used by :nth-child() and friends; matches index i when
 * a != 0 and (i - b) is a non-negative multiple of a (or, for a < 0, when i
 * steps down from b and stays >= 1). `present` marks the pseudo being used. */
typedef struct HpeNth {
    int a; /* step; `odd` = 2, `even` = 2 with b = 0/1 */
    int b; /* offset */
    int present;
} HpeNth;

typedef struct HpeSelector HpeSelector;

typedef struct HpeCompound {
    char *tag; /* NULL or "*" means any element */
    char *id;
    char **classes;
    int class_count;
    size_t class_cap;
    HpeAttrSel *attrs;
    int attr_count;
    size_t attr_cap;
    unsigned pseudo; /* HPE_PS_* bits */
    HpeNth nth_child;      /* :nth-child(an+b) */
    HpeNth nth_last_child; /* :nth-last-child(an+b) */
    HpeNth nth_of_type;    /* :nth-of-type(an+b) */
    HpeNth nth_last_of_type; /* :nth-last-of-type(an+b) */
    HpeSelector *not_list;   /* :not(...) argument (any match fails) */
    int not_count;
    HpeSelector *is_list;    /* :is(...) / :where(...) argument */
    int is_count;
    int is_where;            /* 1 for `:where()`, which contributes no specificity */
    int pseudo_element;      /* HpePseudoElement */
    int comb; /* HpeCombinator for this part (relative to the previous part) */
} HpeCompound;

struct HpeSelector {
    HpeCompound *parts; /* left to right, the last part is the subject */
    int count;
    size_t cap;
    int specificity;
};


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

/**
 * @brief 初始化样式表并载入内置 UA 样式 / Initialise a stylesheet and load the built-in UA stylesheet
 *
 * @details 先把整个结构清零，再把视口默认设为 800x600，最后解析 `hpe_ua_stylesheet()` 的文本，因此新样式表一开始就带有浏览器默认样式。
 *          The struct is zeroed, the viewport defaults to 800x600, and the text from `hpe_ua_stylesheet()` is parsed, so a fresh sheet already carries the user-agent defaults.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 *
 * @note 传 `NULL` 时直接返回 / Returns immediately when `NULL` is passed
 *
 * @see hpe_css_free hpe_css_parse
 */
void hpe_css_init(HpeStylesheet *sheet);

/**
 * @brief 释放样式表持有的全部规则 / Release every rule held by a stylesheet
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 *
 * @note 逐条释放规则后把整个结构清零，可安全重复调用 / Rules are released one by one and the struct is then zeroed, so repeated calls are safe
 *
 * @see hpe_css_init
 */
void hpe_css_free(HpeStylesheet *sheet);

/**
 * @brief 解析一段 CSS 文本并追加到样式表 / Parse CSS text and append it to a stylesheet
 *
 * @details 在栈上分配一个媒体查询栈后调用 `css_parse_into` 从第 0 层开始递归解析，规则按出现顺序追加，不会清空已有规则。
 *          A media-query stack is allocated on the stack and `css_parse_into` recurses from depth 0; rules are appended in source order and existing rules are kept.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     css 待解析的 CSS 文本 / CSS text to parse
 * @param[in]     length `css` 的字节长度 / Byte length of `css`
 *
 * @see css_parse_into hpe_css_init
 */
void hpe_css_parse(HpeStylesheet *sheet, const char *css, size_t length);

/**
 * @brief 设置用于求值 @media 的视口尺寸 / Set the viewport size used to evaluate @media
 *
 * @details 只写入宽高两个字段，不重新解析样式表；规则的 `@media` 条件在层叠时按此尺寸即时求值，因此在重新层叠前调用即可让媒体查询随窗口变化生效。
 *          Only the width and height fields are written and the sheet is not re-parsed; each rule's `@media` conditions are evaluated against this size at cascade time, so calling it before a re-cascade makes media queries follow the window.
 *
 * @param[in,out] sheet 目标样式表 / Target stylesheet
 * @param[in]     vw 视口宽度（像素）/ Viewport width in pixels
 * @param[in]     vh 视口高度（像素）/ Viewport height in pixels
 *
 * @note 传 `NULL` 时直接返回 / Returns immediately when `NULL` is passed
 *
 * @see hpe_css_compute
 */
void hpe_css_set_viewport(HpeStylesheet *sheet, float vw, float vh);

/**
 * @brief 测试单个选择器是否匹配给定节点 / Test whether a standalone selector matches a node
 *
 * @details 把 `sel` 当作可能含逗号的完整选择器文本解析成规则，任一选择器命中即算匹配；组合符会沿节点的祖先与兄弟链回溯。
 *          `sel` is parsed as a full selector list that may contain commas and a hit on any one selector counts as a match; combinators walk the node's ancestors and siblings.
 *
 * @param[in]     node 待测试的 DOM 节点 / DOM node to test
 * @param[in]     sel 选择器文本（可含逗号）/ Selector text, possibly comma-separated
 * @param[in]     len `sel` 的字节长度 / Byte length of `sel`
 *
 * @return 匹配返回 1，否则返回 0 / 1 on a match, 0 otherwise
 *
 * @note 供 `querySelector` 一类接口使用 / Used by interfaces such as `querySelector`
 *
 * @see selector_matches compound_matches
 */
int hpe_css_selector_matches(const HpeDomNode *node, const char *sel, size_t len);

/**
 * @brief 返回一份填好 CSS 初始值的默认样式 / Return a default style seeded with CSS initial values
 *
 * @details 先 `memset` 清零，再依次调用 `style_init_box` 与 `style_init_scalars`，因此所有非零初值（如 `auto`、透明色、单位变换、`font-size: 16px`）都已就位。
 *          The struct is zeroed first, then `style_init_box` and `style_init_scalars` fill in every non-zero initial value such as `auto`, transparent colours, the identity transform and `font-size: 16px`.
 *
 * @return 初始化完毕的样式值（按值返回）/ A fully initialised style returned by value
 *
 * @see style_init_box style_init_scalars
 */
HpeStyle hpe_style_default(void);

/**
 * @brief 解析声明块并写入样式 / Parse a declaration block into a style
 *
 * @details 输入通常是 `style=""` 属性的内容；按 `;` 切分后逐条解析 `属性: 值`，识别并记录 `!important`，非法或空声明被忽略。
 *          The input is normally the content of a `style=""` attribute; it is split on `;` and each `property: value` pair is parsed, `!important` is detected and recorded, and malformed or empty declarations are skipped.
 *
 * @param[in]     text 声明块文本 / Declaration block text
 * @param[in]     length `text` 的字节长度 / Byte length of `text`
 * @param[out]    decl 接收解析结果的样式 / Receives the parsed style
 *
 * @note 进入时先清空 `decl`；`text` 为空时只做清零 / `decl` is cleared on entry; when `text` is empty only the clear happens
 *
 * @see apply_property strip_important
 */
void hpe_css_parse_declarations(const char *text, size_t length, HpeStyle *decl);

/**
 * @brief 计算节点的最终样式（两趟层叠）/ Compute a node's final style with a two-pass cascade
 *
 * @details 第一趟从 `hpe_style_default` 出发并按 `parent` 继承可继承属性，收集所有媒体条件通过且选择器匹配的规则，按（specificity, 源码顺序）排序后依次应用，最后叠加内联 `style=""`（胜过普通样式表声明）。
 *          Pass one starts from `hpe_style_default`, inherits the inheritable properties from `parent`, gathers every rule whose media conditions pass and whose selectors match, applies them sorted by (specificity, source order), and finally overlays the inline `style=""`, which beats a normal stylesheet declaration.
 *          第二趟只处理 `!important` 声明：把它们（含内联）按同一种键排序后仅应用被标记为 important 的位；在 important 层内内联仍胜过样式表，其余由源码顺序决定。
 *          Pass two handles `!important` declarations only: they are sorted by the same key and only the bits flagged important are applied; within the important origin an inline `style=""` still beats a stylesheet, otherwise source order decides.
 *
 * @param[in,out] sheet 样式表；为 `NULL` 时跳过规则匹配 / Stylesheet; rule matching is skipped when `NULL`
 * @param[in]     node 待计算样式的 DOM 节点 / DOM node whose style is computed
 * @param[in]     parent 父节点样式，用于继承；可为 `NULL` / Parent style used for inheritance; may be `NULL`
 * @param[out]    out 接收计算后的样式 / Receives the computed style
 *
 * @note 计算得到的样式保留“已声明”位（`set`/`set2`），供绘制阶段区分显式值与继承值 / The computed style keeps the "declared" bits (`set`/`set2`) so later stages can tell an explicit value from an inherited one
 *
 * @see apply_decl apply_decl_important inherit_parent_style
 */
void hpe_css_compute(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *parent,
                     HpeStyle *out);

/**
 * @brief 计算 `::before` / `::after` 伪元素的计算样式 / Compute a `::before` / `::after` pseudo-element's style
 *
 * @details 从宿主元素的计算样式出发，套用所有选择器命中（末段带同向伪元素）的规则，并叠加宿主元素内联 `style` 的可继承部分；结果保留「已声明」位。
 *          Starting from the host element's computed style, every rule whose selectors end in the matching pseudo-element is applied, and the inheritable part of the host's inline `style` is overlaid; the "declared" bits are kept.
 *
 * @param[in,out] sheet 样式表；为 `NULL` 时只用宿主样式 / Stylesheet; when `NULL` only the host style is used
 * @param[in]     node 宿主元素 / Host element
 * @param[in]     host_style 宿主元素的计算样式 / The host element's computed style
 * @param[in]     which `HPE_PE_BEFORE` 或 `HPE_PE_AFTER` / `HPE_PE_BEFORE` or `HPE_PE_AFTER`
 * @param[out]    out 接收计算后的样式 / Receives the computed style
 *
 * @note 只有末段复合选择器带有同向伪元素的选择器才参与匹配 / Only selectors whose final compound carries the matching pseudo-element take part
 *
 * @see hpe_css_compute
 */
void hpe_css_compute_pseudo(HpeStylesheet *sheet, const HpeDomNode *node, const HpeStyle *host_style,
                            int which, HpeStyle *out);

/**
 * @brief 返回内置的用户代理默认样式表文本 / Return the built-in user-agent stylesheet text
 *
 * @details 这份常量文本为 `html`/`body`、标题、列表、表格、表单控件等元素提供 `display`、`margin`、`font-size` 等浏览器默认值，在 `hpe_css_init` 时被解析进样式表。
 *          This constant text supplies browser-default `display`, `margin`, `font-size` and similar values for `html`/`body`, headings, lists, tables and form controls; it is parsed into the sheet by `hpe_css_init`.
 *
 * @return 以 NUL 结尾的 UA 样式表 CSS 文本 / NUL-terminated CSS text of the user-agent stylesheet
 *
 * @see hpe_css_init
 */
const char *hpe_ua_stylesheet(void);

/**
 * @brief 在元素的内联 `style` 属性中设置一个属性 / Set one property in an element's inline `style` attribute
 *
 * @details 扫描现有 `style` 属性，遇到同名属性就替换其值（值为空则删除），其余声明原样保留；没有同名属性时在末尾追加 `prop: value;`，最后整体写回属性。
 *          The existing `style` attribute is scanned; a declaration with the same name has its value replaced (or removed when the new value is empty) while other declarations are kept verbatim, and when no match exists `prop: value;` is appended before writing the attribute back.
 *
 * @param[in,out] node 目标 DOM 元素 / Target DOM element
 * @param[in]     prop CSS 属性名（小写、带连字符）/ CSS property name, lowercase and dashed
 * @param[in]     value 属性值；空串或 `NULL` 表示删除该属性 / Property value; an empty string or `NULL` removes the property
 *
 * @note 供 JavaScript DOM 桥使用 / Used by the JavaScript DOM bridge
 *
 * @see hpe_css_inline_get inline_append
 */
void hpe_css_inline_set(HpeDomNode *node, const char *prop, const char *value);

/**
 * @brief 读取元素内联 `style` 属性中的一个属性值 / Read one property value from an element's inline `style` attribute
 *
 * @details 按 `;` 切分内联样式，逐个比较小写的属性名，命中后把去除首尾空白的值写入 `out` 并返回 1。
 *          The inline style is split on `;` and property names are compared case-insensitively; on a hit the trimmed value is written to `out` and 1 is returned.
 *
 * @param[in]     node 目标 DOM 元素 / Target DOM element
 * @param[in]     prop 要查询的 CSS 属性名 / CSS property name to look up
 * @param[out]    out 接收属性值的缓冲区 / Buffer that receives the property value
 * @param[in]     out_size 接收缓冲区容量（字节）/ Capacity of the output buffer in bytes
 *
 * @return 找到返回 1，否则返回 0 / 1 when found, 0 otherwise
 *
 * @note 属性值以去空白后的文本原样返回，不做规范化 / The value is returned as trimmed text without normalisation
 *
 * @see hpe_css_inline_set
 */
int hpe_css_inline_get(const HpeDomNode *node, const char *prop, char *out, size_t out_size);

#endif /* HPE_CSS_H */
