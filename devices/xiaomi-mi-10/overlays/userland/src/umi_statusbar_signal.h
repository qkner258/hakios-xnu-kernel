/* iOS 16.5: observe native telephony -> aggregator -> main filter -> UIKit.
 * Opt in before SpringBoard starts: /var/umi-statusbar-signal.trace.
 * Remove that file to stop logging immediately. No placement, priority, width, notch or alpha
 * override and no telephony override. Two exceptions: the view-loss repair near the bottom, when a
 * signal or network-type display item is provably orphaned (data says present, display item says
 * removed) and it re-runs the bar's own data-update entry point once, on the main queue, outside
 * every update (it cannot force on a glyph UIKit wants hidden); and the charging-transient cap,
 * which runs UIKit's own -_collapseBattery two seconds after the bolt appears if UIKit's animation
 * timeout has not (see the charging cap section). Neither changes what UIKit is asked to display.
 * Numeric state and native UIKit identifiers only; never describe telephony objects.
 * Included by inject_sb.c after say(), BLOCK_CALL and the existing cellular hook.
 */
#ifndef UMI_STATUSBAR_SIGNAL_H
#define UMI_STATUSBAR_SIGNAL_H

static bool umi_signal_trace_enabled(void)
{
	return access("/var/umi-statusbar-signal.trace", F_OK) == 0;
}

/* --- what the runtime is asked, asked once ------------------------------
 * A snapshot walks every display item and every field of it (~30 items x ~20 fields,
 * INVESTIGATION-sb-launch-freeze.md §3). sel_registerName and class_getInstanceMethod take the
 * runtime's locks and the same (class, field) pair is asked for thousands of times per snapshot,
 * which is where the 10-50 ms went. Each is resolved once and kept. Only what was asked for
 * before is served from here: nothing new is evaluated, and nothing about what is logged changes.
 * All of these hooks run on the main thread (UIKit's status bar), like the rest of this file, so
 * the caches need no lock. */
#define UMI_SIGNAL_SELS 256
static struct {
	const char *name;       /* a literal in this file */
	SEL sel;
} umi_signal_sels[UMI_SIGNAL_SELS];
static unsigned umi_signal_sels_used;

static SEL umi_signal_sel(const char *name)
{
	for (unsigned i = 0; i < umi_signal_sels_used; i++) {
		if (umi_signal_sels[i].name == name) return umi_signal_sels[i].sel;
	}
	SEL sel = sel_registerName(name);
	if (umi_signal_sels_used < UMI_SIGNAL_SELS) {
		umi_signal_sels[umi_signal_sels_used].name = name;
		umi_signal_sels[umi_signal_sels_used].sel = sel;
		umi_signal_sels_used++;
	}
	return sel;
}

/* One getter of one class, as class_getInstanceMethod() and the arity check gave it. */
enum { UMI_SIGNAL_NO_STRUCT, UMI_SIGNAL_RECT, UMI_SIGNAL_SIZE, UMI_SIGNAL_INSETS };
#define UMI_SIGNAL_LOOKUPS 512
struct umi_signal_lookup {
	Class cls;              /* Nil: the slot is free */
	const char *name;
	SEL sel;
	Method method;          /* NULL: no such method, or not one of two arguments */
	char type;              /* the method's return type (0 if there is none) */
	unsigned char shape;    /* which struct it returns, if it returns one */
};
static struct umi_signal_lookup umi_signal_lookups[UMI_SIGNAL_LOOKUPS];
static struct umi_signal_lookup umi_signal_lookup_spill; /* the table full: resolved, not kept */

static void umi_signal_lookup_fill(struct umi_signal_lookup *entry, Class cls, const char *name)
{
	char type[64] = "";
	Method m;

	entry->name = name;
	entry->sel = umi_signal_sel(name);
	m = class_getInstanceMethod(cls, entry->sel);
	entry->method = m != NULL && method_getNumberOfArguments(m) == 2 ? m : NULL;
	entry->type = 0;
	entry->shape = UMI_SIGNAL_NO_STRUCT;
	if (entry->method == NULL) return;
	method_getReturnType(entry->method, type, sizeof(type));
	entry->type = type[0];
	if (strncmp(type, "{CGRect=", 8) == 0) entry->shape = UMI_SIGNAL_RECT;
	else if (strncmp(type, "{CGSize=", 8) == 0) entry->shape = UMI_SIGNAL_SIZE;
	else if (strncmp(type, "{NSDirectionalEdgeInsets=", strlen("{NSDirectionalEdgeInsets=")) == 0 ||
	    strncmp(type, "{UIEdgeInsets=", strlen("{UIEdgeInsets=")) == 0) entry->shape = UMI_SIGNAL_INSETS;
}

static struct umi_signal_lookup *umi_signal_lookup(Class cls, const char *name)
{
	unsigned slot = (unsigned)((((uintptr_t)cls >> 4) * 31u + ((uintptr_t)name >> 3)) & (UMI_SIGNAL_LOOKUPS - 1));

	for (unsigned n = 0; n < UMI_SIGNAL_LOOKUPS; n++, slot = (slot + 1) & (UMI_SIGNAL_LOOKUPS - 1)) {
		struct umi_signal_lookup *entry = &umi_signal_lookups[slot];

		if (entry->cls == cls && entry->name == name) return entry;
		if (entry->cls != Nil) continue;
		/* cls is written last: until then the slot still reads as free. */
		umi_signal_lookup_fill(entry, cls, name);
		entry->cls = cls;
		return entry;
	}
	umi_signal_lookup_fill(&umi_signal_lookup_spill, cls, name);
	umi_signal_lookup_spill.cls = cls;
	return &umi_signal_lookup_spill;
}

/* One last line per object/category, released with the observed object. No
 * event budget: HOME/CC transitions remain observable for the entire boot. */
static void umi_signal_say(id object, const char *category, const char *format, ...)
{
	static char key;
	char line[1280];
	va_list ap;
	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	if (object == nil) { say("%s", line); return; } /* objc_setAssociatedObject(nil) faults */
	id strings = (id)objc_getClass("NSString");
	id name = ((id (*)(id, SEL, const char *))objc_msgSend)(strings, umi_signal_sel("stringWithUTF8String:"), category);
	id value = ((id (*)(id, SEL, const char *))objc_msgSend)(strings, umi_signal_sel("stringWithUTF8String:"), line);
	id previous = objc_getAssociatedObject(object, &key);
	if (previous == nil) {
		previous = ((id (*)(id, SEL))objc_msgSend)((id)objc_getClass("NSMutableDictionary"), umi_signal_sel("dictionary"));
		objc_setAssociatedObject(object, &key, previous, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	id last = ((id (*)(id, SEL, id))objc_msgSend)(previous, umi_signal_sel("objectForKey:"), name);
	if (((BOOL (*)(id, SEL, id))objc_msgSend)(value, umi_signal_sel("isEqualToString:"), last)) return;
	((void (*)(id, SEL, id, id))objc_msgSend)(previous, umi_signal_sel("setObject:forKey:"), value, name);
	say("%s", line);
}

static id umi_signal_object(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return nil;
	entry = umi_signal_lookup(object_getClass(object), name);
	return entry->method != NULL && entry->type == '@' ?
	    ((id (*)(id, SEL))objc_msgSend)(object, entry->sel) : nil;
}

static long umi_signal_number(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return -1;
	entry = umi_signal_lookup(object_getClass(object), name);
	if (entry->method == NULL) return -1;
	switch (entry->type) {
	case 'B': case 'c': return ((BOOL (*)(id, SEL))objc_msgSend)(object, entry->sel);
	case 'i': return ((int (*)(id, SEL))objc_msgSend)(object, entry->sel);
	case 'I': return ((unsigned (*)(id, SEL))objc_msgSend)(object, entry->sel);
	case 'q': return ((long (*)(id, SEL))objc_msgSend)(object, entry->sel);
	case 'Q': return (long)((unsigned long (*)(id, SEL))objc_msgSend)(object, entry->sel);
	default: return -1; /* absent getter, not a fabricated zero */
	}
}

static const unsigned char *umi_signal_ivar(id object, const char *name, const char *type)
{
	Ivar ivar = object ? class_getInstanceVariable(object_getClass(object), name) : NULL;
	const char *actual = ivar ? ivar_getTypeEncoding(ivar) : NULL;
	return actual && strncmp(actual, type, strlen(type)) == 0 ?
	    (const unsigned char *)(void *)object + ivar_getOffset(ivar) : NULL;
}

static long umi_signal_ivar_number(id object, const char *name)
{
	Ivar ivar = object ? class_getInstanceVariable(object_getClass(object), name) : NULL;
	const char *type = ivar ? ivar_getTypeEncoding(ivar) : NULL;
	const unsigned char *p = ivar ? (const unsigned char *)(void *)object + ivar_getOffset(ivar) : NULL;

	if (type == NULL || p == NULL) return -1;
	switch (type[0]) {
	case 'B': case 'c': return *(const signed char *)p;
	case 'i': return *(const int *)p;
	case 'I': return *(const unsigned *)p;
	case 'q': return *(const long *)p;
	case 'Q': return (long)*(const unsigned long *)p;
	default: return -1;
	}
}

static id umi_signal_ivar_object(id object, const char *name)
{
	const unsigned char *p = umi_signal_ivar(object, name, "@");
	return p ? *(id const *)p : nil;
}

static double umi_signal_double(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return -1.0;
	entry = umi_signal_lookup(object_getClass(object), name);
	if (entry->method != NULL) {
		if (entry->type == 'd') return ((double (*)(id, SEL))objc_msgSend)(object, entry->sel);
		if (entry->type == 'f') return ((float (*)(id, SEL))objc_msgSend)(object, entry->sel);
	}
	return -1.0;
}

/* The network-type items are included so the change-driven lines (state, item-state,
 * display-state, placement) also cover "the 4G glyph alone went missing"; they are logged only on
 * a real change, never per frame. */
static bool umi_signal_identifier(id identifier)
{
	Class plain = objc_getClass("_UIStatusBarCellularItem");
	Class condensed = objc_getClass("_UIStatusBarCellularCondensedItem");
	return identifier != nil &&
	    (identifier == umi_signal_object((id)condensed, "signalStrengthDisplayIdentifier") ||
	     identifier == umi_signal_object((id)condensed, "dualSignalStrengthDisplayIdentifier") ||
	     identifier == umi_signal_object((id)plain, "typeDisplayIdentifier") ||
	     identifier == umi_signal_object((id)condensed, "animatedTypeDisplayIdentifier"));
}

/* Read only UIKit geometry and native identifier strings, never item contents. */
static const char *umi_signal_text(id string)
{
	return string ? ((const char *(*)(id, SEL))objc_msgSend)(string, umi_signal_sel("UTF8String")) : "-";
}

static const char *umi_signal_name(id identifier)
{
	return umi_signal_text(umi_signal_object(identifier, "string"));
}

static hk_rect umi_signal_rect(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return (hk_rect){-1, -1, -1, -1};
	entry = umi_signal_lookup(object_getClass(object), name);
	return entry->method != NULL && entry->shape == UMI_SIGNAL_RECT ?
	    ((hk_rect (*)(id, SEL))objc_msgSend)(object, entry->sel) : (hk_rect){-1, -1, -1, -1};
}

static hk_size umi_signal_size(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return (hk_size){-1, -1};
	entry = umi_signal_lookup(object_getClass(object), name);
	return entry->method != NULL && entry->shape == UMI_SIGNAL_SIZE ?
	    ((hk_size (*)(id, SEL))objc_msgSend)(object, entry->sel) : (hk_size){-1, -1};
}

/* -[_UIStatusBarCellularSignalView intrinsicContentSize] asserts ("Undefined icon size") while
 * _iconSize is still 0, i.e. before -applyUpdate:toDisplayItem: ran. Ask only once it is set. */
static hk_size umi_signal_required_size(id view)
{
	const unsigned char *icon = umi_signal_ivar(view, "_iconSize", "q");
	if (icon != NULL && *(const long *)icon == 0) return (hk_size){-1, -1};
	return umi_signal_size(view, "intrinsicContentSize");
}

/* Both edge-inset structs have four doubles on arm64. */
static hk_rect umi_signal_insets(id object, const char *name)
{
	struct umi_signal_lookup *entry;

	if (object == nil) return (hk_rect){-1, -1, -1, -1};
	entry = umi_signal_lookup(object_getClass(object), name);
	return entry->method != NULL && entry->shape == UMI_SIGNAL_INSETS ?
	    ((hk_rect (*)(id, SEL))objc_msgSend)(object, entry->sel) : (hk_rect){-1, -1, -1, -1};
}

static const char *umi_signal_role(id bar)
{
	for (id p = bar; p != nil; p = umi_signal_object(p, "nextResponder")) {
		const char *name = object_getClassName(p);
		if (strstr(name, "ControlCenter")) return "CC";
		if (strstr(name, "CoverSheet")) return "LOCK";
		if (strstr(name, "HomeScreen")) return "HOME";
	}
	return "STATUS/APP";
}

static void umi_signal_geometry(id bar)
{
	/* Ivar, not the getter: -visualProvider calls the hooked _prepareVisualProviderIfNeeded. */
	const unsigned char *visualp = umi_signal_ivar(bar, "_visualProvider", "@");
	id visual = visualp ? *(id const *)visualp : nil;
	const unsigned char *classp = umi_signal_ivar(bar, "_visualProviderClass", "#");
	Class selected = classp ? *(Class const *)classp : Nil;
	id screen = umi_signal_object(bar, "_effectiveTargetScreen");
	id window = umi_signal_object(bar, "window");
	id scene = umi_signal_object(window, "windowScene");
	id root = umi_signal_object(window, "rootViewController");
	id info = umi_signal_object(screen, "_displayInfoProvider");
	hk_rect bounds = umi_signal_rect(screen, "bounds"), native = umi_signal_rect(screen, "nativeBounds");
	hk_rect display = umi_signal_rect(screen, "_nativeDisplayBounds");
	hk_rect exclusion = umi_signal_rect(umi_signal_object(screen, "_exclusionArea"), "rect");
	hk_rect notch = umi_signal_rect(visual, "notchRect");
	hk_rect safe = umi_signal_insets(window, "safeAreaInsets");
	hk_rect leading = umi_signal_insets(visual, "leadingEdgeInsets");
	hk_rect trailing = umi_signal_insets(visual, "trailingEdgeInsets");
	hk_rect expanded = umi_signal_insets(visual, "expandedEdgeInsets");
	hk_size expected = umi_signal_size((id)object_getClass(visual), "notchSize");
	umi_signal_say(bar, "geometry", "hakios: sb-signal geometry bar=%p role=%s window=%p window-class=%s "
	    "scene=%p scene-class=%s root=%s visual=%s selected=%s mode=%ld expanded=%ld lock=%ld "
	    "screen=%p bounds=%.3f,%.3f,%.3f,%.3f native=%.3f,%.3f,%.3f,%.3f "
	    "display=%.3f,%.3f,%.3f,%.3f scale=%.3f native-scale=%.3f private-scale=%.3f private-native-scale=%.3f "
	    "reference-scale=%.3f reference-width=%.3f artwork=%ld safe=%.3f,%.3f,%.3f,%.3f\n",
	    (void *)bar, umi_signal_role(bar), (void *)window, window ? object_getClassName(window) : "-",
	    (void *)scene, scene ? object_getClassName(scene) : "-", root ? object_getClassName(root) : "-",
	    visual ? object_getClassName(visual) : "-", selected ? class_getName(selected) : "-",
	    umi_signal_number(bar, "mode"), umi_signal_number(visual, "expanded"), umi_signal_number(visual, "onLockScreen"),
	    (void *)screen, bounds.x, bounds.y, bounds.width, bounds.height, native.x, native.y, native.width, native.height,
	    display.x, display.y, display.width, display.height, umi_signal_double(screen, "scale"),
	    umi_signal_double(screen, "nativeScale"), umi_signal_double(screen, "_scale"), umi_signal_double(screen, "_nativeScale"),
	    umi_signal_double((id)object_getClass(visual), "referenceScreenScale"), umi_signal_double(visual, "referenceWidth"),
	    umi_signal_number(info, "artworkSubtype"), safe.x, safe.y, safe.width, safe.height);
	umi_signal_say(bar, "notch", "hakios: sb-signal notch bar=%p exclusion=%.3f,%.3f,%.3f,%.3f "
	    "notch=%.3f,%.3f,%.3f,%.3f class-notch=%.3f,%.3f class-native-width=%.3f "
	    "leading-insets=%.3f,%.3f,%.3f,%.3f trailing-insets=%.3f,%.3f,%.3f,%.3f "
	    "expanded-insets=%.3f,%.3f,%.3f,%.3f spacing=%.3f corner=%.3f\n",
	    (void *)bar, exclusion.x, exclusion.y, exclusion.width, exclusion.height, notch.x, notch.y, notch.width, notch.height,
	    expected.width, expected.height, umi_signal_double((id)object_getClass(visual), "nativeDisplayWidth"),
	    leading.x, leading.y, leading.width, leading.height, trailing.x, trailing.y, trailing.width, trailing.height,
	    expanded.x, expanded.y, expanded.width, expanded.height, umi_signal_double(visual, "itemSpacing"),
	    umi_signal_double(visual, "cornerRadius"));
}

static void umi_signal_region(id region)
{
	id bar = umi_signal_object(region, "statusBar");
	id container = umi_signal_object(region, "containerItem");
	hk_rect bounds = umi_signal_rect(container, "_ui_bounds");
	hk_rect frame = umi_signal_rect(container, "_ui_frame");
	id items = umi_signal_object(region, "displayItems");
	umi_signal_say(region, "region", "hakios: sb-signal region bar=%p role=%s region=%p name=%s enabled=%ld tokens=%ld "
	    "frame=%.3f,%.3f,%.3f,%.3f bounds=%.3f,%.3f,%.3f,%.3f items=%ld frame-overflow-disabled=%ld\n",
	    (void *)bar, umi_signal_role(bar), (void *)region, umi_signal_text(umi_signal_object(region, "identifier")),
	    umi_signal_number(region, "isEnabled"), umi_signal_number(umi_signal_object(region, "disablingTokens"), "count"),
	    frame.x, frame.y, frame.width, frame.height, bounds.x, bounds.y, bounds.width, bounds.height,
	    umi_signal_number(items, "count"), umi_signal_number(region, "disableItemFrameBasedOverflow"));
	for (long i = 0; i < umi_signal_number(items, "count"); i++) {
		id peer = ((id (*)(id, SEL, long))objc_msgSend)(items, umi_signal_sel("objectAtIndex:"), i);
		id view = umi_signal_ivar_object(peer, "_view");
		hk_rect f = umi_signal_rect(umi_signal_object(peer, "layoutItem"), "_ui_frame");
		hk_size required = umi_signal_required_size(view);
		umi_signal_say(peer, "region-item", "hakios: sb-signal region-item bar=%p region=%p display=%p name=%s item=%s "
		    "priority=%ld enabled=%ld placement-enabled=%ld required=%.3f,%.3f frame=%.3f,%.3f,%.3f,%.3f\n",
		    (void *)bar, (void *)region, (void *)peer, umi_signal_name(umi_signal_object(peer, "identifier")),
		    object_getClassName(umi_signal_object(peer, "item")), umi_signal_number(peer, "priority"),
		    umi_signal_number(peer, "isEnabled"), umi_signal_number(umi_signal_object(peer, "placement"), "isEnabled"),
		    required.width, required.height, f.x, f.y, f.width, f.height);
	}
}

/* 20F66 display items are not in their view's superview chain. Resolve the
 * item through _UIStatusBar's state table, including after removal animations. */
static void umi_signal_log_placement(id state, const char *event, const void *address)
{
	id bar = umi_signal_object(state, "statusBar");
	id display = umi_signal_object(state, "displayItem");
	id view = umi_signal_ivar_object(display, "_view");
	id window = umi_signal_object(bar, "window");
	id placement_state = umi_signal_object(state, "currentPlacementState");
	id region = umi_signal_object(placement_state, "region");
	id blocker = nil;
	double alpha = 1;
	for (id p = view; p != nil; p = umi_signal_object(p, "superview")) {
		long hidden = umi_signal_number(p, "isHidden");
		double a = umi_signal_double(p, "alpha");
		if (blocker == nil && (hidden == 1 || a == 0)) blocker = p;
		alpha *= a;
	}
	char who[160];
	/* The four plain scalars _updateStatuses reads to pick the animation type. None of
	 * them runs any code, so they are safe mid-update (unlike isFulfilled/canBeEnabled).
	 * Looked up by name; -1 means the ivar is not there, not a fabricated zero. */
	umi_signal_say(state, event, "hakios: sb-signal placement event=%s caller=%s bar=%p window=%s role=%s state=%p display=%p "
	    "index=%ld preferred=%ld data-enabled=%ld enabled=%ld display-enabled=%ld visible=%ld "
	    "view-hidden=%ld placement-enabled=%ld region=%p region-enabled=%ld region-tokens=%ld relations=%ld "
	    "was-enabled=%ld was-visible=%ld enability=%ld visibility=%ld "
	    "view=%p view-alpha=%.3f display-alpha=%.3f view-factor=%.3f dynamic=%ld dynamic-alpha=%.3f "
	    "effective-alpha=%.3f blocker=%p blocker-class=%s\n",
	    event, caller(address, who, sizeof(who)), (void *)bar,
	    window ? object_getClassName(window) : "-", umi_signal_role(bar), (void *)state, (void *)display,
	    umi_signal_number(state, "currentPlacementStateIndex"), umi_signal_number(state, "preferredPlacementStateIndex"),
	    umi_signal_number(state, "dataEnabled"), umi_signal_number(state, "isEnabledIgnoringAnimations"),
	    umi_signal_number(display, "isEnabled"), umi_signal_number(display, "visible"),
	    umi_signal_number(view, "isHidden"), umi_signal_number(umi_signal_object(placement_state, "placement"), "isEnabled"),
	    (void *)region, umi_signal_number(region, "isEnabled"),
	    umi_signal_number(umi_signal_object(region, "disablingTokens"), "count"),
	    -1L /* areRelationsFulfilled asserts mid-update (11-13) */,
	    umi_signal_ivar_number(state, "_wasEnabled"), umi_signal_ivar_number(state, "_wasVisible"),
	    umi_signal_ivar_number(state, "_enabilityStatus"), umi_signal_ivar_number(state, "_visibilityStatus"),
	    (void *)view,
	    umi_signal_double(view, "alpha"), umi_signal_double(display, "alpha"),
	    umi_signal_double(display, "viewAlpha"), umi_signal_number(display, "dynamicallyHidden"),
	    umi_signal_double(display, "dynamicHidingAlpha"), alpha,
	    (void *)blocker, blocker ? object_getClassName(blocker) : "-");
}

static void umi_signal_relations(id state, long index, id relations)
{
	for (long j = 0; j < umi_signal_number(relations, "count"); j++) {
		id relation = ((id (*)(id, SEL, long))objc_msgSend)(relations, umi_signal_sel("objectAtIndex:"), j);
		id peer = umi_signal_object(relation, "itemState");
		id placement = umi_signal_object(relation, "placement");
		umi_signal_say(relation, "relation", "hakios: sb-signal relation state=%p index=%ld relation=%p class=%s type=%ld "
		    "fulfilled=%ld requires-all=%ld peer=%p peer-item=%s peer-name=%s peer-index=%ld requirement=%ld "
		    "target-placement=%p target-name=%s target-enabled=%ld\n",
		    (void *)state, index, (void *)relation, object_getClassName(relation), umi_signal_number(relation, "type"),
		    -1L /* isFulfilled evaluates peers' -isEnabled, which asserts mid-update (SpringBoard crash 11-13): derive from relation-peer lines */, umi_signal_number(relation, "requiresAll"), (void *)peer,
		    peer ? object_getClassName(umi_signal_object(peer, "item")) : "-",
		    umi_signal_name(umi_signal_object(peer, "identifier")), umi_signal_number(peer, "currentPlacementStateIndex"),
		    umi_signal_number(relation, "requirement"), (void *)placement,
		    umi_signal_name(umi_signal_object(placement, "identifier")), umi_signal_number(placement, "isEnabled"));
		id peers = umi_signal_object(relation, "itemStates");
		for (long k = 0; k < umi_signal_number(peers, "count"); k++) {
			id member = ((id (*)(id, SEL, long))objc_msgSend)(peers, umi_signal_sel("objectAtIndex:"), k);
			char key[80];
			snprintf(key, sizeof(key), "relation-peer-%p", (void *)member);
			umi_signal_say(relation, key, "hakios: sb-signal relation-peer state=%p index=%ld relation=%p peer=%p "
			    "item=%s name=%s peer-index=%ld enabled=%ld\n", (void *)state, index, (void *)relation,
			    (void *)member, object_getClassName(umi_signal_object(member, "item")),
			    umi_signal_name(umi_signal_object(member, "identifier")), umi_signal_number(member, "currentPlacementStateIndex"),
			    umi_signal_number(member, "isEnabledIgnoringAnimations"));
		}
		umi_signal_relations(state, index, umi_signal_object(relation, "relations"));
	}
}

static void umi_signal_candidates(id self)
{
	id placements = umi_signal_object(self, "placementStates");
	for (long i = 0; i < umi_signal_number(placements, "count"); i++) {
		id candidate = ((id (*)(id, SEL, long))objc_msgSend)(placements, umi_signal_sel("objectAtIndex:"), i);
		id region = umi_signal_object(candidate, "region");
		umi_signal_say(candidate, "candidate", "hakios: sb-signal candidate state=%p index=%ld region=%p region-enabled=%ld "
		    "placement-enabled=%ld can-enable=%ld relations=%ld priority=%ld item=%s name=%s\n",
		    (void *)self, i, (void *)region, umi_signal_number(region, "isEnabled"),
		    umi_signal_number(umi_signal_object(candidate, "placement"), "isEnabled"),
		    -1L, -1L /* both evaluate relations -> peers' -isEnabled asserts mid-update (11-13 crash) */,
		    umi_signal_number(candidate, "priority"), object_getClassName(umi_signal_object(self, "item")),
		    umi_signal_name(umi_signal_object(self, "identifier")));
		umi_signal_relations(self, i, umi_signal_object(candidate, "relations"));
	}
}

static void umi_signal_placement_changed(id self, long before, const char *event, const void *address)
{
	if (!umi_signal_trace_enabled() || before == umi_signal_number(self, "currentPlacementStateIndex") ||
	    !umi_signal_identifier(umi_signal_object(self, "identifier"))) return;
	umi_signal_log_placement(self, event, address);
	umi_signal_candidates(self);
}

/* These methods write _currentPlacementStateIndex directly in 20F66. A hook
 * on setCurrentPlacementStateIndex: misses both actual eviction paths. */
static void (*umi_signal_original_next_placement)(id, SEL);
static void umi_signal_next_placement(id self, SEL cmd)
{
	long before = umi_signal_number(self, "currentPlacementStateIndex");
	if (umi_signal_trace_enabled() && umi_signal_identifier(umi_signal_object(self, "identifier"))) {
		umi_signal_log_placement(self, "overflow-before", __builtin_return_address(0));
		umi_signal_candidates(self);
	}
	umi_signal_original_next_placement(self, cmd);
	umi_signal_placement_changed(self, before, "overflow-next", __builtin_return_address(0));
}

static BOOL (*umi_signal_original_next_if_needed)(id, SEL);
static BOOL umi_signal_next_if_needed(id self, SEL cmd)
{
	long before = umi_signal_number(self, "currentPlacementStateIndex");
	if (umi_signal_trace_enabled() && umi_signal_identifier(umi_signal_object(self, "identifier")))
		umi_signal_candidates(self);
	BOOL result = umi_signal_original_next_if_needed(self, cmd);
	umi_signal_placement_changed(self, before, "disabled-next", __builtin_return_address(0));
	return result;
}

static id (*umi_signal_original_overflow)(id, SEL);
static id umi_signal_overflow(id self, SEL cmd)
{
	id result = umi_signal_original_overflow(self, cmd);
	if (!umi_signal_trace_enabled()) return result;
	id bar = umi_signal_object(self, "statusBar");
	umi_signal_region(self);
	umi_signal_say(self, "overflow", "hakios: sb-signal overflow bar=%p role=%s region=%p name=%s victim=%p "
	    "victim-name=%s item=%s priority=%ld\n", (void *)bar, umi_signal_role(bar), (void *)self,
	    umi_signal_text(umi_signal_object(self, "identifier")), (void *)result,
	    umi_signal_name(umi_signal_object(result, "identifier")), result ? object_getClassName(umi_signal_object(result, "item")) : "-",
	    umi_signal_number(result, "priority"));
	return result;
}

static void umi_signal_region_token(id region, unsigned long token, const char *event, const void *address)
{
	if (!umi_signal_trace_enabled()) return;
	char who[160];
	umi_signal_say(region, "region-token", "hakios: sb-signal region-token event=%s region=%p bar=%p name=%s "
	    "token=%lu tokens=%ld enabled=%ld caller=%s\n", event, (void *)region,
	    (void *)umi_signal_object(region, "statusBar"), umi_signal_text(umi_signal_object(region, "identifier")), token,
	    umi_signal_number(umi_signal_object(region, "disablingTokens"), "count"), umi_signal_number(region, "isEnabled"),
	    caller(address, who, sizeof(who)));
}

static void (*umi_signal_original_region_disable)(id, SEL, unsigned long);
static void umi_signal_region_disable(id self, SEL cmd, unsigned long token)
{
	long before = umi_signal_number(umi_signal_object(self, "disablingTokens"), "count");
	umi_signal_original_region_disable(self, cmd, token);
	if (before != umi_signal_number(umi_signal_object(self, "disablingTokens"), "count"))
		umi_signal_region_token(self, token, "disable", __builtin_return_address(0));
}

static void (*umi_signal_original_region_enable)(id, SEL, unsigned long);
static void umi_signal_region_enable(id self, SEL cmd, unsigned long token)
{
	long before = umi_signal_number(umi_signal_object(self, "disablingTokens"), "count");
	umi_signal_original_region_enable(self, cmd, token);
	if (before != umi_signal_number(umi_signal_object(self, "disablingTokens"), "count"))
		umi_signal_region_token(self, token, "enable", __builtin_return_address(0));
}

/* Defined with the known-bar table below; the placement hook runs long before it in the file. */
static void umi_signal_charging_watch(id placement, BOOL enabled);

static void (*umi_signal_original_placement_enabled)(id, SEL, BOOL);
static void umi_signal_placement_enabled(id self, SEL cmd, BOOL enabled)
{
	long before = umi_signal_number(self, "isEnabled");
	umi_signal_original_placement_enabled(self, cmd, enabled);
	if (before == umi_signal_number(self, "isEnabled")) return;
	/* The cap is logic, not logging: it must still run if the trace file was removed after
	 * install (the hooks stay installed). */
	umi_signal_charging_watch(self, enabled);
	if (!umi_signal_trace_enabled()) return;
	char who[160];
	umi_signal_say(self, "placement-enabled", "hakios: sb-signal placement-enabled placement=%p name=%s "
	    "enabled=%ld priority=%ld caller=%s\n", (void *)self, umi_signal_name(umi_signal_object(self, "identifier")),
	    umi_signal_number(self, "isEnabled"), umi_signal_number(self, "priority"), caller(__builtin_return_address(0), who, sizeof(who)));
}

static void umi_signal_snapshot(id bar, const char *event)
{
	if (!umi_signal_trace_enabled()) return;
	umi_signal_geometry(bar);
	id regions = umi_signal_object(umi_signal_object(bar, "regions"), "allValues");
	for (long i = 0; i < umi_signal_number(regions, "count"); i++) {
		id region = ((id (*)(id, SEL, long))objc_msgSend)(regions, umi_signal_sel("objectAtIndex:"), i);
		umi_signal_region(region);
	}
	id states = umi_signal_object(umi_signal_object(bar, "displayItemStates"), "allValues");
	for (long i = 0; i < umi_signal_number(states, "count"); i++) {
		id state = ((id (*)(id, SEL, long))objc_msgSend)(states, umi_signal_sel("objectAtIndex:"), i);
		id display = umi_signal_object(state, "displayItem");
		id placement = umi_signal_object(state, "currentPlacementState");
		id view = umi_signal_ivar_object(display, "_view");
		hk_size required = umi_signal_required_size(view);
		umi_signal_say(state, "state", "hakios: sb-signal state bar=%p state=%p display=%p name=%s item=%s "
		    "index=%ld data-enabled=%ld enabled=%ld region=%p priority=%ld placement-enabled=%ld "
		    "display-enabled=%ld visible=%ld hidden=%ld alpha=%.3f required=%.3f,%.3f tokens=%ld\n",
		    (void *)bar, (void *)state, (void *)display, umi_signal_name(umi_signal_object(state, "identifier")),
		    object_getClassName(umi_signal_object(state, "item")), umi_signal_number(state, "currentPlacementStateIndex"),
		    umi_signal_number(state, "dataEnabled"), umi_signal_number(state, "isEnabledIgnoringAnimations"),
		    (void *)umi_signal_object(placement, "region"), umi_signal_number(placement, "priority"),
		    umi_signal_number(placement, "isEnabled"), umi_signal_number(display, "isEnabled"),
		    umi_signal_number(display, "visible"), umi_signal_number(view, "isHidden"), umi_signal_double(view, "alpha"),
		    required.width, required.height, umi_signal_number(umi_signal_object(display, "disablingTokens"), "count"));
		umi_signal_candidates(state);
		if (umi_signal_identifier(umi_signal_object(state, "identifier")))
			umi_signal_log_placement(state, event, NULL);
	}
}

static void (*umi_signal_original_bar_layout)(id, SEL);
static void umi_signal_bar_layout(id self, SEL cmd)
{
	umi_signal_original_bar_layout(self, cmd);
	umi_signal_snapshot(self, "layout-end");
}

static void (*umi_signal_original_prepare)(id, SEL);
static void umi_signal_prepare(id self, SEL cmd)
{
	umi_signal_original_prepare(self, cmd);
	if (umi_signal_trace_enabled()) umi_signal_geometry(self);
}

/* --- view-loss reconciliation (v29i) -------------------------------------
 * UIKit only flips a display item from removed back to added inside the per-item data
 * update (-[_UIStatusBarDisplayItemState updateWithData:styleAttributes:] -> _updateStatuses ->
 * prepareAnimation: type 1). When the charging transient times out, _collapseBattery runs
 * _updateDisplayedItemsWithData:nil:nil: and that pass can restore currentPlacementStateIndex to
 * 0 without ever running the item's data update, so the display item stays enabled=NO
 * (preferred=-1, display-enabled=0) and the region never re-admits it. Closing Control Center
 * reaches the same state. The item then waits for the next real data update, which may not come.
 * The repair is the pass that update would have run, with the bar's own current inputs.
 * -[_UIStatusBar updateWithData:] is deliberately NOT used: it re-enters the visual provider and
 * can replay the charging bolt. The one relation-evaluating call (isEnabledIgnoringAnimations) is
 * made only when no update is on the stack and this state's own data update has finished; see
 * umi_signal_orphaned.
 */

/* _statusBarFlags.updatingStates is bit 19 (0x80000) of the flags word: set for the whole body
 * of -[_UIStatusBar _updateDisplayedItemsWithData:styleAttributes:extraAnimations:] (0x18919f58c
 * orr / 0x18919f638 and). A plain bit test cannot evaluate a relation. When the ivar cannot be
 * read we treat the bar as idle: the dispatch_async hop is the first guard and this file's own
 * call is covered by umi_signal_reconciling. */
static bool umi_signal_bar_updating(id bar)
{
	Ivar flags = bar ? class_getInstanceVariable(object_getClass(bar), "_statusBarFlags") : NULL;
	const char *type = flags ? ivar_getTypeEncoding(flags) : NULL;
	const unsigned char *p;
	if (type == NULL || type[0] != '{') return false;
	p = (const unsigned char *)(const void *)bar + ivar_getOffset(flags);
	return (*(const unsigned *)p & 0x00080000u) != 0;
}

/* Argument-count check, the same idea as the arity check of umi_signal_lookup_fill (it needs any
 * arity, so it cannot use that table; its SEL comes from the cache). */
static bool umi_signal_implements(id object, const char *name, unsigned arguments)
{
	Method m = object ? class_getInstanceMethod(object_getClass(object), umi_signal_sel(name)) : NULL;
	return m != NULL && method_getNumberOfArguments(m) == arguments + 2;
}

/* --- charging transient cap (v29i) ---------------------------------------
 * The bolt is opened by -[_UIStatusBar _updateDataForBatteryCharging:] and closed by the visual
 * provider's -_collapseBattery when the add animation times out. While it is up the trailing
 * region cannot hold the battery icon (24.3 -> 34.1 pt) plus the bolt plus the signal (17 pt) and
 * network-type (15.3 pt) items, so UIKit evicts those two by priority (1102/1104 vs 1111/1300),
 * and on a slow boot the timeout has been seen 52 s late instead of 1-2 s
 * (INVESTIGATION-statusbar-loss-3.md). Cap the transient: one generation and one dispatch_after
 * per rising edge; on expiry, if no enable/disable happened since, run the pass UIKit runs on its
 * own timeout. No priority change, no orphan-criteria change, no new view.
 */
#define UMI_SIGNAL_KNOWN_BARS 8
#define UMI_SIGNAL_CHARGING_CAP 2 /* seconds; the bolt's intended lifetime */
static id umi_signal_known_bars[UMI_SIGNAL_KNOWN_BARS];
static unsigned umi_signal_known_bar_next;
static unsigned umi_signal_charging_generation;

/* The animation hook is the only place that hands this file a bar. One retain per slot and a
 * release on eviction keep the deferred block from dereferencing a bar UIKit has since freed; at
 * most UMI_SIGNAL_KNOWN_BARS objects are held. Main thread only, like the rest of this file. */
static void umi_signal_remember_bar(id bar)
{
	unsigned slot;
	id old;

	if (bar == nil) return;
	for (unsigned i = 0; i < UMI_SIGNAL_KNOWN_BARS; i++)
		if (umi_signal_known_bars[i] == bar) return;
	slot = umi_signal_known_bar_next++ % UMI_SIGNAL_KNOWN_BARS;
	old = umi_signal_known_bars[slot];
	umi_signal_known_bars[slot] = ((id (*)(id, SEL))objc_msgSend)(bar, umi_signal_sel("retain"));
	if (old != nil) ((void (*)(id, SEL))objc_msgSend)(old, umi_signal_sel("release"));
}

/* -_collapseBattery is UIKit's own end of the transient: it disables the charging placement and
 * re-runs _updateDisplayedItemsWithData:, which re-admits the evicted items. The arity check
 * makes an absent/renamed selector a silent skip. Bars whose update pass is on the stack are left
 * alone; the bar's own timeout then finishes the job. */
static void umi_signal_charging_collapse(unsigned generation)
{
	for (unsigned i = 0; i < UMI_SIGNAL_KNOWN_BARS; i++) {
		id bar = umi_signal_known_bars[i];
		id visual;

		if (bar == nil || umi_signal_bar_updating(bar) || umi_signal_object(bar, "window") == nil) continue;
		visual = umi_signal_ivar_object(bar, "_visualProvider");
		if (!umi_signal_implements(visual, "_collapseBattery", 0)) continue;
		if (umi_signal_trace_enabled())
			say("hakios: sb-signal collapse-check bar=%p role=%s gen=%u\n",
			    (void *)bar, umi_signal_role(bar), generation);
		((void (*)(id, SEL))objc_msgSend)(visual, umi_signal_sel("_collapseBattery"));
	}
}

/* One rising edge bumps the generation and arms one block; a falling edge (or a later rising
 * edge) bumps it again, which is what makes an already-armed block a no-op. */
static void umi_signal_charging_watch(id placement, BOOL enabled)
{
	unsigned generation;

	if (strcmp(umi_signal_name(umi_signal_object(placement, "identifier")), "chargingDisplayIdentifier") != 0) return;
	generation = ++umi_signal_charging_generation;
	if (!enabled) return;
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(UMI_SIGNAL_CHARGING_CAP * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		if (umi_signal_charging_generation != generation) return;
		umi_signal_charging_collapse(generation);
	});
}

/* A display item is orphaned iff UIKit's state says it must be there while the display item
 * says it is not. dataEnabled is a plain ivar read. isEnabledIgnoringAnimations (0x1891c53cc) is
 * _dataEnabled && currentPlacementState.isEnabled, and the placement state's -isEnabled is
 * canBeEnabled && areRelationsFulfilled (0x1891e0840), so it does reach peers' -isEnabled through
 * -isFulfilled. That walk is what asserted mid-update on 11-13, so it is only made once this
 * state's own data update has finished: _dataUpdateStatus (+0x58) is 0/1 during a pass and 2 at
 * its tail. Reading that scalar cannot assert; if it is not 2 we simply do nothing this time.
 * A genuinely disabled placement still fails the test, so a glyph UIKit wants hidden is safe. */
static bool umi_signal_orphaned(id bar, id identifier, id *offender)
{
	id state, item;
	long updated;

	if (bar == nil || identifier == nil ||
	    !umi_signal_implements(bar, "stateForDisplayItemWithIdentifier:", 1)) return false;
	state = ((id (*)(id, SEL, id))objc_msgSend)(bar, umi_signal_sel("stateForDisplayItemWithIdentifier:"), identifier);
	if (state == nil) return false;
	if (umi_signal_number(state, "dataEnabled") != 1) return false;
	updated = umi_signal_ivar_number(state, "_dataUpdateStatus"); /* -1: ivar absent, do not block */
	if (updated >= 0 && updated != 2) return false;
	if (umi_signal_number(state, "isEnabledIgnoringAnimations") != 1) return false;
	item = umi_signal_object(state, "displayItem");
	if (item == nil || umi_signal_number(item, "isEnabled") != 0) return false;
	*offender = item;
	return true;
}

/* Two tries per bar per orphaned episode, then one reconcile-failed line and silence until the
 * item is seen coherent again (which re-arms the budget for a later episode). Slots are matched
 * by bar pointer only, so a stale slot is a pointer compare, never a dereference. */
#define UMI_SIGNAL_RECONCILE_BARS 8
#define UMI_SIGNAL_RECONCILE_TRIES 2
struct umi_signal_reconcile_slot {
	id bar;
	id item;
	unsigned tries;
	bool failed;
};
static struct umi_signal_reconcile_slot umi_signal_reconcile_slots[UMI_SIGNAL_RECONCILE_BARS];
static unsigned umi_signal_reconcile_next_slot;
static bool umi_signal_reconciling; /* this file's own update is on the stack */

static struct umi_signal_reconcile_slot *umi_signal_reconcile_slot(id bar, id item)
{
	struct umi_signal_reconcile_slot *free_slot = NULL;

	for (unsigned i = 0; i < UMI_SIGNAL_RECONCILE_BARS; i++) {
		struct umi_signal_reconcile_slot *slot = &umi_signal_reconcile_slots[i];
		if (slot->bar == bar) {
			if (slot->item != item) { slot->item = item; slot->tries = 0; slot->failed = false; }
			return slot;
		}
		if (free_slot == NULL && slot->bar == nil) free_slot = slot;
	}
	if (free_slot == NULL)
		free_slot = &umi_signal_reconcile_slots[umi_signal_reconcile_next_slot++ % UMI_SIGNAL_RECONCILE_BARS];
	free_slot->bar = bar;
	free_slot->item = item;
	free_slot->tries = 0;
	free_slot->failed = false;
	return free_slot;
}

static void umi_signal_reconcile_forget(id bar)
{
	for (unsigned i = 0; i < UMI_SIGNAL_RECONCILE_BARS; i++) {
		struct umi_signal_reconcile_slot *slot = &umi_signal_reconcile_slots[i];
		if (slot->bar != bar) continue;
		slot->bar = nil;
		slot->item = nil;
		slot->tries = 0;
		slot->failed = false;
	}
}

/* Main queue only, one block per completed top-level status-bar animation: never a timer and
 * never per-frame. Skips detached/reuse-pool bars, bars whose update is on the stack, and early
 * boot bars without data or style. */
static void umi_signal_reconcile(id bar)
{
	Class cellular = objc_getClass("_UIStatusBarCellularCondensedItem");
	id signal, dual, type, animated;
	id single_offender = nil, dual_offender = nil, type_offender = nil, animated_offender = nil;
	bool single_bad, dual_bad, type_bad, animated_bad;
	id offender, data, style;
	struct umi_signal_reconcile_slot *slot;

	if (!umi_signal_trace_enabled() || bar == nil || umi_signal_reconciling) return;
	if (umi_signal_bar_updating(bar)) return;
	if (umi_signal_object(bar, "window") == nil) return;
	if (!umi_signal_implements(bar, "currentData", 0) || !umi_signal_implements(bar, "styleAttributes", 0) ||
	    !umi_signal_implements(bar, "_updateDisplayedItemsWithData:styleAttributes:extraAnimations:", 3)) return;

	signal = umi_signal_object((id)cellular, "signalStrengthDisplayIdentifier");
	dual = umi_signal_object((id)cellular, "dualSignalStrengthDisplayIdentifier");
	type = umi_signal_object((id)cellular, "typeDisplayIdentifier");
	animated = umi_signal_object((id)cellular, "animatedTypeDisplayIdentifier");
	single_bad = umi_signal_orphaned(bar, signal, &single_offender);
	dual_bad = umi_signal_orphaned(bar, dual, &dual_offender);
	type_bad = umi_signal_orphaned(bar, type, &type_offender);
	animated_bad = umi_signal_orphaned(bar, animated, &animated_offender);
	offender = single_offender ? single_offender : dual_offender ? dual_offender : type_offender ? type_offender : animated_offender;
	if (offender == nil) { umi_signal_reconcile_forget(bar); return; }

	slot = umi_signal_reconcile_slot(bar, offender);
	if (slot->tries >= UMI_SIGNAL_RECONCILE_TRIES) {
		if (!slot->failed) {
			slot->failed = true;
			umi_signal_say(offender, "reconcile-failed", "hakios: sb-signal reconcile-failed bar=%p display=%p "
			    "single=%d dual=%d type=%d animated-type=%d tries=%u\n", (void *)bar, (void *)offender,
			    single_bad, dual_bad, type_bad, animated_bad, slot->tries);
		}
		return;
	}
	data = umi_signal_object(bar, "currentData");
	style = umi_signal_object(bar, "styleAttributes");
	if (data == nil || style == nil) return;

	slot->tries++;
	umi_signal_say(bar, "reconcile", "hakios: sb-signal reconcile bar=%p data=%p style=%p display=%p "
	    "single=%d dual=%d type=%d animated-type=%d attempt=%u\n", (void *)bar, (void *)data, (void *)style,
	    (void *)offender, single_bad, dual_bad, type_bad, animated_bad, slot->tries);
	umi_signal_reconciling = true;
	((void (*)(id, SEL, id, id, id))objc_msgSend)(bar,
	    umi_signal_sel("_updateDisplayedItemsWithData:styleAttributes:extraAnimations:"), data, style, nil);
	umi_signal_reconciling = false;
}

/* 20F66 addTotalCompletionHandler: uses dispatch_group_notify on the main
 * queue after this animation and all subanimations complete (0x1892430c8).
 * Observe there so a surviving container during removal is not mistaken for
 * a visible icon. No polling or delayed guesses about animation duration. */
static void (*umi_signal_original_animation)(id, SEL, id);
static void umi_signal_animation(id self, SEL cmd, id bar)
{
	bool top = umi_signal_object(self, "parentAnimation") == nil;

	/* Recorded even with the trace off: the charging cap is behaviour, not logging. */
	if (top && bar != nil) umi_signal_remember_bar(bar);
	if (top && umi_signal_trace_enabled() && bar != nil) {
		/* This is C: a block does not retain a captured id. The bar is held from here until the
		 * main-queue repair has run (dispatch_group_notify fires the handler once; a handler that
		 * never fires leaks one bar instead of leaving a dangling pointer). */
		id held = ((id (*)(id, SEL))objc_msgSend)(bar, umi_signal_sel("retain"));
		((void (*)(id, SEL, void (^)(void)))objc_msgSend)(self, umi_signal_sel("addTotalCompletionHandler:"), ^{
			umi_signal_snapshot(held, "animation-end");
			/* One runloop turn after the pass settled currentPlacementStateIndex:
			 * _updateDisplayedItemsWithData: must not be re-entered from inside one. */
			dispatch_async(dispatch_get_main_queue(), ^{
				umi_signal_reconcile(held);
				((void (*)(id, SEL))objc_msgSend)(held, umi_signal_sel("release"));
			});
		});
	}
	umi_signal_original_animation(self, cmd, bar);
}

static void (*umi_signal_original_fixup)(id, SEL);
static void umi_signal_fixup(id self, SEL cmd)
{
	umi_signal_original_fixup(self, cmd);
	umi_signal_snapshot(self, "fixup");
}

static id umi_signal_superview(id object)
{
	return umi_signal_object(object, "superview");
}

/* Numeric-only view/item observation. This runs from the existing signal-mode hook, so it
 * also samples long-lived HOME views after a delta update has omitted cellularEntry. */
static void umi_signal_log_view_state(id signal_view, long requested, long applied)
{
	if (!umi_signal_trace_enabled()) return;
	umi_signal_say(signal_view, "view-state", "hakios: sb-signal view-state view=%p class=%s requested=%ld applied=%ld "
	    "active-bars=%ld bars=%ld mode=%ld hidden=%ld alpha=%.3f\n",
	    (void *)signal_view, object_getClassName(signal_view), requested, applied,
	    umi_signal_number(signal_view, "numberOfActiveBars"), umi_signal_number(signal_view, "numberOfBars"),
	    umi_signal_number(signal_view, "signalMode"), umi_signal_number(signal_view, "isHidden"),
	    umi_signal_double(signal_view, "alpha"));
}

/* Only itemIsEnabled[46] is read; no private string/identity fields or fixed offsets. */
static unsigned umi_signal_mask(const unsigned char *data)
{
	return data ? (unsigned)(!!data[4] | (!!data[5] << 1) | (!!data[9] << 2) | (!!data[10] << 3)) : 0;
}

static void (*umi_signal_original_update)(id, SEL, int, id, id, void *, void *);
static void umi_signal_update(id self, SEL cmd, int item, id data, id sim, void *getter, void *setter)
{
	umi_signal_original_update(self, cmd, item, data, sim, getter, setter);
	if (!umi_signal_trace_enabled()) return;
	const unsigned char *raw = umi_signal_ivar(self, "_data", "{?=\"itemIsEnabled\"[46B]");
	int result = getter ? ((int (*)(void *))((struct block_layout *)getter)->invoke)(getter) : -1;
	umi_signal_say(self, item == 4 ? "agg-primary" : "agg-secondary", "hakios: sb-signal agg item=%d domain=%d sim=%d radio=%ld dead=%ld service=%ld cell-service=%ld "
	    "bars=%ld max=%ld rat=%ld providing=%ld preferred=%ld raw-bars=%d enabled=%d mask=%x\n",
	    item, data != nil, sim != nil, umi_signal_number(data, "isCellularRadioCapabilityEnabled"),
	    umi_signal_number(data, "isRadioModuleDead"), umi_signal_number(sim, "serviceState"),
	    umi_signal_number(sim, "cellularServiceState"), umi_signal_number(sim, "signalStrengthBars"),
	    umi_signal_number(sim, "maxSignalStrengthBars"), umi_signal_number(sim, "dataNetworkType"),
	    umi_signal_number(sim, "isProvidingDataConnection"), umi_signal_number(sim, "isPreferredForDataConnections"),
	    result, raw && item >= 0 && item < 46 ? raw[item] : -1, umi_signal_mask(raw));
}

static void (*umi_signal_original_compose)(id, SEL, unsigned char *);
static void umi_signal_compose(id self, SEL cmd, unsigned char *data)
{
	bool trace = umi_signal_trace_enabled();
	unsigned before = trace ? umi_signal_mask(data) : 0;
	umi_signal_original_compose(self, cmd, data);
	if (!trace) return;
	const unsigned char *disabled = umi_signal_ivar(self, "_itemIsDisabled", "[46B]");
	const unsigned char *cloak = umi_signal_ivar(self, "_telephonyAndBluetoothCloaked", "B");
	const unsigned char *all = umi_signal_ivar(self, "_allButBatteryCloaked", "B");
	umi_signal_say(self, "main", "hakios: sb-signal main mask=%x->%x disabled=%x cloak=%d all-cloak=%d\n", before,
	    umi_signal_mask(data), umi_signal_mask(disabled), cloak ? *cloak : -1, all ? *all : -1);
}

static id (*umi_signal_original_convert)(id, SEL, const unsigned char *);
static id umi_signal_convert(id self, SEL cmd, const unsigned char *data)
{
	id result = umi_signal_original_convert(self, cmd, data);
	if (!umi_signal_trace_enabled()) return result;
	id provider = umi_signal_object(self, "inProcessStateProvider");
	id entry = umi_signal_object(result, "cellularEntry");
	umi_signal_say(self, "ui", "hakios: sb-signal ui view=%p provider=%s mask=%x entry=%d enabled=%ld bars=%ld rat=%ld status=%ld\n",
	    (void *)self, provider ? object_getClassName(provider) : "server", umi_signal_mask(data), entry != nil,
	    umi_signal_number(entry, "isEnabled"), umi_signal_number(entry, "displayValue"), umi_signal_number(entry, "type"),
	    umi_signal_number(entry, "status"));
	return result;
}

/* convertData:fromReferenceData: ends with makeUpdateFromData: (20F66,
 * 0x18919dc9c). A missing entry there means unchanged, not absent. Observe
 * both retained stores after UIKit has applied the update instead. */
static void (*umi_signal_original_aggregate)(id, SEL, id);
static void umi_signal_aggregate(id self, SEL cmd, id update)
{
	umi_signal_original_aggregate(self, cmd, update);
	if (!umi_signal_trace_enabled()) return;
	id current = umi_signal_object(umi_signal_object(self, "currentData"), "cellularEntry");
	id stored = umi_signal_object(umi_signal_object(self, "currentAggregatedData"), "cellularEntry");
	/* Ivar, not the getter: -visualProvider calls the hooked _prepareVisualProviderIfNeeded. */
	const unsigned char *visualp = umi_signal_ivar(self, "_visualProvider", "@");
	id visual = visualp ? *(id const *)visualp : nil;
	id window = umi_signal_object(self, "window");
	umi_signal_say(self, "retained", "hakios: sb-signal retained bar=%p window=%s visual=%s mode=%ld expanded=%ld "
	    "current=%d current-enabled=%ld current-bars=%ld stored=%d enabled=%ld bars=%ld status=%ld\n",
	    (void *)self, window ? object_getClassName(window) : "-",
	    visual ? object_getClassName(visual) : "-", umi_signal_number(self, "mode"),
	    umi_signal_number(visual, "expanded"), current != nil,
	    umi_signal_number(current, "isEnabled"), umi_signal_number(current, "displayValue"),
	    stored != nil, umi_signal_number(stored, "isEnabled"),
	    umi_signal_number(stored, "displayValue"), umi_signal_number(stored, "status"));
}

/* These are the state-changing points below _dataFromLegacyData:. A sparse
 * _UIStatusBarData update may have no cellularEntry, so observing only the
 * converter cannot tell whether an existing HOME view was left intact. */
static void (*umi_signal_original_active_bars)(id, SEL, long);
static void (*umi_signal_original_bars)(id, SEL, long);
static void (*umi_signal_original_display_enabled)(id, SEL, BOOL);
static id (*umi_signal_original_cellular_apply)(id, SEL, id, id);
static id (*umi_signal_original_condensed_apply)(id, SEL, id, id);
static void (*umi_signal_original_layout)(id, SEL);

static void umi_signal_layout(id self, SEL cmd)
{
	umi_signal_original_layout(self, cmd);
	if (!umi_signal_trace_enabled()) return;
	id window = umi_signal_object(self, "window");
	id parent = umi_signal_superview(self);
	hk_rect frame = ((hk_rect (*)(id, SEL))objc_msgSend)(self, umi_signal_sel("frame"));
	id layer = umi_signal_object(self, "layer");
	long hidden = 0;
	double alpha = 1;
	for (id p = self; p != nil; p = umi_signal_superview(p)) {
		hidden |= umi_signal_number(p, "isHidden");
		alpha *= umi_signal_double(p, "alpha");
	}
	umi_signal_say(self, "layout", "hakios: sb-signal layout view=%p class=%s parent=%s window=%s frame=%.2f,%.2f,%.2f,%.2f "
	    "hidden=%ld alpha=%.3f icon-size=%ld active-bars=%ld bars=%ld mode=%ld layers=%ld\n",
	    (void *)self, object_getClassName(self), parent ? object_getClassName(parent) : "-",
	    window ? object_getClassName(window) : "-",
	    frame.x, frame.y, frame.width, frame.height, hidden, alpha, umi_signal_number(self, "iconSize"),
	    umi_signal_number(self, "numberOfActiveBars"), umi_signal_number(self, "numberOfBars"),
	    umi_signal_number(self, "signalMode"), umi_signal_number(umi_signal_object(layer, "sublayers"), "count"));
}

static void umi_signal_active_bars(id self, SEL cmd, long value)
{
	umi_signal_original_active_bars(self, cmd, value);
	umi_signal_log_view_state(self, -1, -1);
}

static void umi_signal_bars(id self, SEL cmd, long value)
{
	umi_signal_original_bars(self, cmd, value);
	umi_signal_log_view_state(self, -1, -1);
}

static void umi_signal_display_enabled(id self, SEL cmd, BOOL enabled)
{
	umi_signal_original_display_enabled(self, cmd, enabled);
	if (!umi_signal_trace_enabled() || !umi_signal_identifier(umi_signal_object(self, "identifier"))) return;
	id view = umi_signal_ivar_object(self, "_view");
	umi_signal_say(self, "display-state", "hakios: sb-signal display-state display=%p class=%s enabled=%d view=%p view-class=%s\n",
	    (void *)self, object_getClassName(self), enabled, (void *)view,
	    view ? object_getClassName(view) : "-");
}

static void umi_signal_log_item(id self, id update, id display_item)
{
	if (!umi_signal_trace_enabled() || !umi_signal_identifier(umi_signal_object(display_item, "identifier"))) return;
	id view = umi_signal_ivar_object(display_item, "_view");
	id identifier = umi_signal_object(display_item, "identifier");
	id entry = ((id (*)(id, SEL, id))objc_msgSend)(self,
	    umi_signal_sel("entryForDisplayItemWithIdentifier:"), identifier);
	id bar = umi_signal_object(self, "statusBar");
	id state = ((id (*)(id, SEL, id))objc_msgSend)(bar,
	    umi_signal_sel("stateForDisplayItemWithIdentifier:"), identifier);
	/* The Small/Flat views belong to the dual-SIM item, not the single-SIM
	 * HOME glyph (20F66 -[_UIStatusBarDualCellularSignalView _commonInit]). */
	bool single_signal = identifier == umi_signal_object((id)object_getClass(self), "signalStrengthDisplayIdentifier");
	bool dual_signal = identifier == umi_signal_object((id)objc_getClass("_UIStatusBarCellularCondensedItem"),
	    "dualSignalStrengthDisplayIdentifier");
	umi_signal_say(display_item, "item-state", "hakios: sb-signal item-state item=%p class=%s display=%p enabled=%ld visible=%ld "
	    "bar=%p single-signal=%d dual-signal=%d changed=%ld entry=%d entry-enabled=%ld entry-bars=%ld "
	    "data-enabled=%ld placement=%ld tokens=%ld "
	    "view=%p view-hidden=%ld view-alpha=%.3f active-bars=%ld bars=%ld mode=%ld\n",
	    (void *)self, object_getClassName(self), (void *)display_item,
	    umi_signal_number(display_item, "isEnabled"), umi_signal_number(display_item, "visible"),
	    (void *)bar, single_signal, dual_signal, umi_signal_number(update, "dataChanged"), entry != nil,
	    umi_signal_number(entry, "isEnabled"), umi_signal_number(entry, "displayValue"),
	    umi_signal_number(state, "dataEnabled"), umi_signal_number(state, "currentPlacementStateIndex"),
	    umi_signal_number(umi_signal_object(display_item, "disablingTokens"), "count"),
	    (void *)view, umi_signal_number(view, "isHidden"),
	    umi_signal_double(view, "alpha"), umi_signal_ivar_number(view, "_numberOfActiveBars"),
	    umi_signal_ivar_number(view, "_numberOfBars"), umi_signal_ivar_number(view, "_signalMode"));
}

static id umi_signal_cellular_apply(id self, SEL cmd, id update, id display_item)
{
	id result = umi_signal_original_cellular_apply(self, cmd, update, display_item);
	umi_signal_log_item(self, update, display_item);
	return result;
}

static id umi_signal_condensed_apply(id self, SEL cmd, id update, id display_item)
{
	id result = umi_signal_original_condensed_apply(self, cmd, update, display_item);
	umi_signal_log_item(self, update, display_item);
	return result;
}

static Class (*umi_signal_original_selection)(id, SEL, id, id);
static Class umi_signal_selection(id self, SEL cmd, id screen, id info)
{
	Class selected = umi_signal_original_selection(self, cmd, screen, info);
	if (!umi_signal_trace_enabled()) return selected;
	hk_rect native = umi_signal_rect(screen, "_nativeDisplayBounds");
	hk_rect exclusion = umi_signal_rect(umi_signal_object(screen, "_exclusionArea"), "rect");
	umi_signal_say(screen, "selection", "hakios: sb-signal selection screen=%p selector-class=%s selected=%s "
	    "native-display=%.3f,%.3f,%.3f,%.3f exclusion=%.3f,%.3f,%.3f,%.3f scale=%.3f native-scale=%.3f\n",
	    (void *)screen, class_getName((Class)self), selected ? class_getName(selected) : "-",
	    native.x, native.y, native.width, native.height, exclusion.x, exclusion.y, exclusion.width, exclusion.height,
	    umi_signal_double(screen, "_scale"), umi_signal_double(screen, "_nativeScale"));
	return selected;
}

/* Check runtime argument/return kinds before installing, including pointer arguments. */
static IMP umi_signal_wrap_class(Class c, const char *cls, const char *sel, const char *kinds, IMP replacement)
{
	Method m = c ? class_getInstanceMethod(c, umi_signal_sel(sel)) : NULL;
	char type[128];
	if (!m || method_getNumberOfArguments(m) != strlen(kinds) - 1) goto unavailable;
	method_getReturnType(m, type, sizeof(type));
	if (type[0] != kinds[0]) goto unavailable;
	for (unsigned i = 0; i < strlen(kinds) - 1; i++) {
		method_getArgumentType(m, i, type, sizeof(type));
		const char *p = type;
		while (*p && strchr("rnNoORV", *p)) p++;
		if (*p != kinds[i + 1]) goto unavailable;
	}
	IMP original = method_getImplementation(m);
	method_setImplementation(m, replacement);
	say("hakios: sb-signal installed %s %s\n", cls, sel);
	return original;
unavailable:
	say("hakios: sb-signal unavailable %s %s\n", cls, sel);
	return NULL;
}

static IMP umi_signal_wrap(const char *cls, const char *sel, const char *kinds, IMP replacement)
{
	return umi_signal_wrap_class(objc_getClass(cls), cls, sel, kinds, replacement);
}

static void umi_statusbar_signal_trace_install(void)
{
	if (!umi_signal_trace_enabled()) return;
	say("hakios: sb-signal trace enabled; state changes only; no event budget; mask bits=signal,secondary-signal,data,secondary-data\n");
	umi_signal_original_selection = (void *)umi_signal_wrap_class(
	    object_getClass((id)objc_getClass("_UIStatusBarVisualProvider_Split")),
	    "+_UIStatusBarVisualProvider_Split", "visualProviderSubclassForScreen:visualProviderInfo:",
	    "#@:@@", (IMP)umi_signal_selection);
	umi_signal_original_update = (void *)umi_signal_wrap("SBStatusBarStateAggregator",
	    "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:", "v@:i@@@@", (IMP)umi_signal_update);
	umi_signal_original_compose = (void *)umi_signal_wrap("SBMainStatusBarStateProvider",
	    "_composePostDataFromAggregatorData:", "v@:^", (IMP)umi_signal_compose);
	umi_signal_original_convert = (void *)umi_signal_wrap("UIStatusBar_Modern",
	    "_dataFromLegacyData:", "@@:^", (IMP)umi_signal_convert);
	umi_signal_original_aggregate = (void *)umi_signal_wrap("_UIStatusBar",
	    "_updateWithAggregatedData:", "v@:@", (IMP)umi_signal_aggregate);
	umi_signal_original_active_bars = (void *)umi_signal_wrap("_UIStatusBarSignalView",
	    "setNumberOfActiveBars:", "v@:q", (IMP)umi_signal_active_bars);
	umi_signal_original_bars = (void *)umi_signal_wrap("_UIStatusBarSignalView",
	    "setNumberOfBars:", "v@:q", (IMP)umi_signal_bars);
	umi_signal_original_display_enabled = (void *)umi_signal_wrap("_UIStatusBarDisplayItem",
	    "setEnabled:", "v@:B", (IMP)umi_signal_display_enabled);
	umi_signal_original_cellular_apply = (void *)umi_signal_wrap("_UIStatusBarCellularItem",
	    "applyUpdate:toDisplayItem:", "@@:@@", (IMP)umi_signal_cellular_apply);
	umi_signal_original_condensed_apply = (void *)umi_signal_wrap("_UIStatusBarCellularCondensedItem",
	    "applyUpdate:toDisplayItem:", "@@:@@", (IMP)umi_signal_condensed_apply);
	umi_signal_original_layout = (void *)umi_signal_wrap("_UIStatusBarCellularSignalView",
	    "layoutSubviews", "v@:", (IMP)umi_signal_layout);
	umi_signal_original_next_placement = (void *)umi_signal_wrap("_UIStatusBarDisplayItemState",
	    "updateToNextEnabledPlacement", "v@:", (IMP)umi_signal_next_placement);
	umi_signal_original_next_if_needed = (void *)umi_signal_wrap("_UIStatusBarDisplayItemState",
	    "_updateToNextPlacementStateIfNeeded", "B@:", (IMP)umi_signal_next_if_needed);
	umi_signal_original_overflow = (void *)umi_signal_wrap("_UIStatusBarRegion",
	    "overflowedDisplayItem", "@@:", (IMP)umi_signal_overflow);
	umi_signal_original_fixup = (void *)umi_signal_wrap("_UIStatusBar",
	    "_fixupDisplayItemAttributes", "v@:", (IMP)umi_signal_fixup);
	umi_signal_original_bar_layout = (void *)umi_signal_wrap("_UIStatusBar",
	    "layoutSubviews", "v@:", (IMP)umi_signal_bar_layout);
	umi_signal_original_prepare = (void *)umi_signal_wrap("_UIStatusBar",
	    "_prepareVisualProviderIfNeeded", "v@:", (IMP)umi_signal_prepare);
	umi_signal_original_animation = (void *)umi_signal_wrap("_UIStatusBarAnimation",
	    "performForStatusBar:", "v@:@", (IMP)umi_signal_animation);
	umi_signal_original_region_disable = (void *)umi_signal_wrap("_UIStatusBarRegion",
	    "disableWithToken:", "v@:Q", (IMP)umi_signal_region_disable);
	umi_signal_original_region_enable = (void *)umi_signal_wrap("_UIStatusBarRegion",
	    "enableWithToken:", "v@:Q", (IMP)umi_signal_region_enable);
	umi_signal_original_placement_enabled = (void *)umi_signal_wrap("_UIStatusBarDisplayItemPlacement",
	    "setEnabled:", "v@:B", (IMP)umi_signal_placement_enabled);
}
#endif
