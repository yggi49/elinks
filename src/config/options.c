/* Options variables manipulation core */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <string.h>

#include "elinks.h"

#include "bfu/dialog.h"
#include "cache/cache.h"
#include "config/conf.h"
#include "config/dialogs.h"
#include "config/domain.h"
#include "config/options.h"
#include "config/opttypes.h"
#include "dialogs/status.h"
#include "document/document.h"
#include "globhist/globhist.h"
#include "intl/charsets.h"
#include "intl/gettext/libintl.h"
#include "main/main.h" /* shrink_memory() */
#include "main/select.h"
#include "network/connection.h"
#include "session/session.h"
#include "terminal/color.h"
#include "terminal/screen.h"
#include "terminal/terminal.h"
#include "util/color.h"
#include "util/error.h"
#include "util/memory.h"
#include "util/string.h"
#include "viewer/text/draw.h"


/* TODO? In the past, covered by shadow and legends, remembered only by the
 * ELinks Elders now, options were in hashes (it was not for a long time, after
 * we started to use dynamic options lists and before we really started to use
 * hierarchic options). Hashes might be swift and deft, but they had a flaw and
 * the flaw showed up as the fatal flaw. They were unsorted, and it was
 * unfriendly to mere mortal users, without pasky's options handlers in their
 * brain, but their own poor-written software. And thus pasky went and rewrote
 * options so that they were in lists from then to now and for all the ages of
 * men, to the glory of mankind. However, one true hero may arise in future
 * fabulous and implement possibility to have both lists and hashes for trees,
 * as it may be useful for some supernatural entities. And when that age will
 * come... */


/* TODO: We should remove special case for root options and use some auxiliary
 * (struct option *) instead. This applies to bookmarks, global history and
 * listbox items as well, though. --pasky */

static INIT_LIST_OF(struct option, options_root_tree);

static struct option options_root = INIT_OPTION(
	/* name: */	"",
	/* flags: */	0,
	/* type: */	OPT_TREE,
	/* min, max: */	0, 0,
	/* value: */	&options_root_tree,
	/* desc: */	"",
	/* capt: */	NULL
);

struct option *config_options;
struct option *cmdline_options;

static void add_opt_rec(struct option *, unsigned char *, struct option *);
static void free_options_tree(LIST_OF(struct option) *, int recursive);

#ifdef CONFIG_DEBUG
/* Detect ending '.' (and some others) in options captions.
 * It will emit a message in debug mode only. --Zas */

#define bad_punct(c) (c != ')' && c != '>' && !isquote(c) && ispunct(c))

static void
check_caption(unsigned char *caption)
{
	int len;
	unsigned char c;

	if (!caption) return;

	len = strlen(caption);
	if (!len) return;

	c = caption[len - 1];
	if (isspace(c) || bad_punct(c))
		DBG("bad char at end of caption [%s]", caption);

#ifdef CONFIG_NLS
	caption = gettext(caption);
	len = strlen(caption);
	if (!len) return;

	c = caption[len - 1];
	if (isspace(c) || bad_punct(c))
		DBG("bad char at end of i18n caption [%s]", caption);
#endif
}

#undef bad_punct

static void
check_description(unsigned char *desc)
{
	int len;
	unsigned char c;

	if (!desc) return;

	len = strlen(desc);
	if (!len) return;

	c = desc[len - 1];
	if (isspace(c))
		DBG("bad char at end of description [%s]", desc);

#ifdef CONFIG_NLS
	desc = gettext(desc);
	len = strlen(desc);
	if (!len) return;

	if (ispunct(c) != ispunct(desc[len - 1]))
		DBG("punctuation char possibly missing at end of i18n description [%s]", desc);

	c = desc[len - 1];
	if (isspace(c))
		DBG("bad char at end of i18n description [%s]", desc);
#endif
}

/*! @relates option */
static void
debug_check_option_syntax(struct option *option)
{
	if (!option) return;
	check_caption(option->capt);
	check_description(option->desc);
}

#else
#define debug_check_option_syntax(option)
#endif


/**********************************************************************
 Options interface
**********************************************************************/

/* If option name contains dots, they are created as "categories" - first,
 * first category is retrieved from list, taken as a list, second category
 * is retrieved etc. */

/* Ugly kludge */
static int no_autocreate = 0;

/** Get record of option of given name, or NULL if there's no such option.
 *
 * If the specified option is an ::OPT_ALIAS, this function returns the
 * alias, rather than the option to which the alias refers.  It must
 * work this way because the alias may have the ::OPT_ALIAS_NEGATE flag.
 * Instead, if the caller tries to read or set the value of the alias,
 * the functions associated with ::OPT_ALIAS will forward the operation
 * to the underlying option.  However, see indirect_option().
 *
 * @relates option */
struct option *
get_opt_rec(struct option *tree, const unsigned char *name_)
{
	struct option *option;
	unsigned char *aname = stracpy(name_);
	unsigned char *name = aname;
	unsigned char *sep;

	if (!aname) return NULL;

	/* We iteratively call get_opt_rec() each for path_elements-1, getting
	 * appropriate tree for it and then resolving [path_elements]. */
	if ((sep = strrchr((const char *)name, '.'))) {
		*sep = '\0';

		tree = get_opt_rec(tree, name);
		if (!tree || tree->type != OPT_TREE || tree->flags & OPT_HIDDEN) {
#if 0
			DBG("ERROR in get_opt_rec() crawl: %s (%d) -> %s",
			      name, tree ? tree->type : -1, sep + 1);
#endif
			mem_free(aname);
			return NULL;
		}

		*sep = '.';
		name = sep + 1;
	}

	foreach (option, *tree->value.tree) {
		if (option->name && !strcmp(option->name, name)) {
			mem_free(aname);
			return option;
		}
	}

	if (tree && tree->flags & OPT_AUTOCREATE && !no_autocreate) {
		struct option *template_ = get_opt_rec(tree, "_template_");

		assertm(template_ != NULL, "Requested %s should be autocreated but "
			"%.*s._template_ is missing!", name_, sep - name_,
			name_);
		if_assert_failed {
			mem_free(aname);
			return NULL;
		}

		/* We will just create the option and return pointer to it
		 * automagically. And, we will create it by cloning _template_
		 * option. By having _template_ OPT_AUTOCREATE and _template_
		 * inside, you can have even multi-level autocreating. */

		option = copy_option(template_, 0);
		if (!option) {
			mem_free(aname);
			return NULL;
		}
		mem_free_set(&option->name, stracpy(name));

		add_opt_rec(tree, "", option);

		mem_free(aname);
		return option;
	}

	mem_free(aname);
	return NULL;
}

/** Get record of option of given name, or NULL if there's no such option. But
 * do not create the option if it doesn't exist and there's autocreation
 * enabled.
 * @relates option */
struct option *
get_opt_rec_real(struct option *tree, const unsigned char *name)
{
	struct option *opt;

	no_autocreate = 1;
	opt = get_opt_rec(tree, name);
	no_autocreate = 0;
	return opt;
}

/** If @a opt is an alias, return the option to which it refers.
 *
 * @warning Because the alias may have the ::OPT_ALIAS_NEGATE flag,
 * the caller must not access the value of the returned option as if
 * it were also the value of the alias.  However, it is safe to access
 * flags such as ::OPT_MUST_SAVE and ::OPT_DELETED.
 *
 * @relates option */
struct option *
indirect_option(struct option *alias)
{
	struct option *real;

	if (alias->type != OPT_ALIAS) return alias; /* not an error */

	real = get_opt_rec(config_options, alias->value.string);
	assertm(real != NULL, "%s aliased to unknown option %s!",
		alias->name, alias->value.string);
	if_assert_failed return alias;

	return real;
}

/** Fetch pointer to value of certain option. It is guaranteed to never return
 * NULL. Note that you are supposed to use wrapper get_opt().
 * @relates option */
union option_value *
get_opt_(
#ifdef CONFIG_DEBUG
	 unsigned char *file, int line, enum option_type option_type,
#endif
	 struct option *tree, unsigned char *name, struct session *ses)
{
	struct option *opt = NULL;

	/* If given a session and the option is shadowed in that session's
	 * options tree, return the shadow. */
	if (ses && ses->option)
		opt = get_opt_rec_real(ses->option, name);

	/* If given a session, no session-specific option was found, and the
	 * option has a shadow in the domain tree that matches the current
	 * document in that session, return that shadow. */
	if (!opt && ses)
		opt = get_domain_option_from_session(name, ses);

	/* Else, return the real option. */
	if (!opt)
		opt = get_opt_rec(tree, name);

#ifdef CONFIG_DEBUG
	errfile = file;
	errline = line;
	if (!opt) elinks_internal("Attempted to fetch nonexisting option %s!", name);

	/* Various sanity checks. */
	if (option_type != opt->type)
		DBG("get_opt_*(\"%s\") @ %s:%d: call with wrapper for %s for option of type %s",
		    name, file, line,
		    get_option_type_name(option_type),
		    get_option_type_name(opt->type));

	switch (opt->type) {
	case OPT_TREE:
		if (!opt->value.tree)
			elinks_internal("Option %s has no value!", name);
		break;
	case OPT_ALIAS:
		elinks_internal("Invalid use of alias %s for option %s!",
				name, opt->value.string);
		break;
	case OPT_STRING:
		if (!opt->value.string)
			elinks_internal("Option %s has no value!", name);
		break;
	case OPT_BOOL:
	case OPT_INT:
		if (opt->value.number < opt->min
		    || opt->value.number > opt->max)
			elinks_internal("Option %s has invalid value %d!", name, opt->value.number);
		break;
	case OPT_LONG:
		if (opt->value.big_number < opt->min
		    || opt->value.big_number > opt->max)
			elinks_internal("Option %s has invalid value %ld!", name, opt->value.big_number);
		break;
	case OPT_COMMAND:
		if (!opt->value.command)
			elinks_internal("Option %s has no value!", name);
		break;
	case OPT_CODEPAGE: /* TODO: check these too. */
	case OPT_LANGUAGE:
	case OPT_COLOR:
		break;
	}
#endif

	return &opt->value;
}

/*! @relates option */
static void
add_opt_sort(struct option *tree, struct option *option, int abi)
{
	LIST_OF(struct option) *cat = tree->value.tree;
	LIST_OF(struct listbox_item) *bcat = &tree->box_item->child;
	struct option *pos;

	/* The list is empty, just add it there. */
	if (list_empty(*cat)) {
		add_to_list(*cat, option);
		if (abi) add_to_list(*bcat, option->box_item);

	/* This fits as the last list entry, add it there. This
	 * optimizes the most expensive BUT most common case ;-). */
	} else if ((option->type != OPT_TREE
		    || ((struct option *) cat->prev)->type == OPT_TREE)
		   && strcmp(((struct option *) cat->prev)->name,
			     option->name) <= 0) {
append:
		add_to_list_end(*cat, option);
		if (abi) add_to_list_end(*bcat, option->box_item);

	/* At the end of the list is tree and we are ordinary. That's
	 * clear case then. */
	} else if (option->type != OPT_TREE
		   && ((struct option *) cat->prev)->type == OPT_TREE) {
		goto append;

	/* Scan the list linearly. This could be probably optimized ie.
	 * to choose direction based on the first letter or so. */
	} else {
		struct listbox_item *bpos = (struct listbox_item *) bcat;

		foreach (pos, *cat) {
			/* First move the box item to the current position but
			 * only if the position has not been marked as deleted
			 * and actually has a box_item -- else we will end up
			 * 'overflowing' and causing assertion failure. */
			if (!(pos->flags & OPT_DELETED) && pos->box_item) {
				bpos = bpos->next;
				assert(bpos != (struct listbox_item *) bcat);
			}

			if ((option->type != OPT_TREE
			     || pos->type == OPT_TREE)
			    && strcmp(pos->name, option->name) <= 0)
				continue;

			/* Ordinary options always sort behind trees. */
			if (option->type != OPT_TREE
			    && pos->type == OPT_TREE)
				continue;

			/* The (struct option) add_at_pos() can mess up the
			 * order so that we add the box_item to itself, so
			 * better do it first. */

			/* Always ensure that them _template_ options are
			 * before anything else so a lonely autocreated option
			 * (with _template_ options set to invisible) will be
			 * connected with an upper corner (ascii: `-) instead
			 * of a rotated T (ascii: +-) when displaying it. */
			if (option->type == pos->type
			    && *option->name <= '_'
			    && !strcmp(pos->name, "_template_")) {
				if (abi) add_at_pos(bpos, option->box_item);
				add_at_pos(pos, option);
				break;
			}

			if (abi) add_at_pos(bpos->prev, option->box_item);
			add_at_pos(pos->prev, option);
			break;
		}

		assert(pos != (struct option *) cat);
		assert(bpos != (struct listbox_item *) bcat);
	}
}

/** Add option to tree.
 * @relates option */
static void
add_opt_rec(struct option *tree, unsigned char *path, struct option *option)
{
	int abi = 0;

	assert(path && option && tree);
	if (*path) tree = get_opt_rec(tree, path);

	assertm(tree != NULL, "Missing option tree for '%s'", path);
	if (!tree->value.tree) return;

	object_nolock(option, "option");

	if (option->box_item && option->name && !strcmp(option->name, "_template_"))
		option->box_item->visible = get_opt_bool("config.show_template", NULL);

	if (tree->flags & OPT_AUTOCREATE && !option->desc) {
		struct option *template_ = get_opt_rec(tree, "_template_");

		assert(template_);
		option->desc = template_->desc;
	}

	option->root = tree;

	abi = (tree->box_item && option->box_item);

	if (abi) {
		/* The config_root tree is a just a placeholder for the
		 * box_items, it actually isn't a real box_item by itself;
		 * these ghosts are indicated by the fact that they have
		 * NULL @next. */
		if (tree->box_item->next) {
			option->box_item->depth = tree->box_item->depth + 1;
		}
	}

	if (tree->flags & OPT_SORT) {
		add_opt_sort(tree, option, abi);

	} else {
		add_to_list_end(*tree->value.tree, option);
		if (abi) add_to_list_end(tree->box_item->child, option->box_item);
	}

	update_hierbox_browser(&option_browser);
}

/*! @relates option */
static inline struct listbox_item *
init_option_listbox_item(struct option *option)
{
	struct listbox_item *item = mem_calloc(1, sizeof(*item));

	if (!item) return NULL;

	init_list(item->child);
	item->visible = 1;
	item->udata = option;
	item->type = (option->type == OPT_TREE) ? BI_FOLDER : BI_LEAF;

	return item;
}

/*! @relates option */
struct option *
add_opt(struct option *tree, unsigned char *path, unsigned char *capt,
	unsigned char *name, enum option_flags flags, enum option_type type,
	long min, long max, longptr_T value, unsigned char *desc)
{
	struct option *option = mem_calloc(1, sizeof(*option));

	if (!option) return NULL;

	option->name = stracpy(name);
	if (!option->name) {
		mem_free(option);
		return NULL;
	}
	option->flags = (flags | OPT_ALLOC);
	option->type = type;
	option->min = min;
	option->max = max;
	option->capt = capt;
	option->desc = desc;

	debug_check_option_syntax(option);

	/* XXX: For allocated values we allocate in the add_opt_<type>() macro.
	 * This involves OPT_TREE and OPT_STRING. */
	switch (type) {
		case OPT_TREE:
			if (!value) {
				mem_free(option);
				return NULL;
			}
			option->value.tree = (LIST_OF(struct option) *) value;
			break;
		case OPT_STRING:
			if (!value) {
				mem_free(option);
				return NULL;
			}
			option->value.string = (unsigned char *) value;
			break;
		case OPT_ALIAS:
			option->value.string = (unsigned char *) value;
			break;
		case OPT_BOOL:
		case OPT_INT:
		case OPT_CODEPAGE:
			option->value.number = (int) value;
			break;
		case OPT_LONG:
			option->value.big_number = (long) value; /* FIXME: cast from void * */
			break;
		case OPT_COLOR:
			decode_color((unsigned char *) value, strlen((unsigned char *) value),
					&option->value.color);
			break;
		case OPT_COMMAND:
			option->value.command = (void *) value;
			break;
		case OPT_LANGUAGE:
			break;
	}

	if (option->type != OPT_ALIAS
	    && ((tree->flags & OPT_LISTBOX) || (option->flags & OPT_LISTBOX))) {
		option->box_item = init_option_listbox_item(option);
		if (!option->box_item) {
			mem_free(option);
			return NULL;
		}
	}

	add_opt_rec(tree, path, option);
	return option;
}

/*! @relates option */
static void
done_option(struct option *option)
{
	switch (option->type) {
		case OPT_STRING:
			mem_free_if(option->value.string);
			break;
		case OPT_TREE:
			mem_free_if(option->value.tree);
			break;
		default:
			break;
	}

	if (option->box_item)
		done_listbox_item(&option_browser, option->box_item);

	if (option->flags & OPT_ALLOC) {
		mem_free_if(option->name);
		mem_free(option);
	} else if (!option->capt) {
		/* We are probably dealing with a built-in autocreated option
		 * that will be attempted to be deleted when shutting down. */
		/* Clear it so nothing will be done later. */
		memset(option, 0, sizeof(*option));
	}
}

/* The namespace may start to seem a bit chaotic here; it indeed is, maybe the
 * function names above should be renamed and only macros should keep their old
 * short names. */
/* The simple rule I took as an apologize is that functions which take already
 * completely filled (struct option *) have long name and functions which take
 * only option specs have short name. */

/*! @relates option */
static void
delete_option_do(struct option *option, int recursive)
{
	if (option->next) {
		del_from_list(option);
		option->prev = option->next = NULL;
	}

	if (recursive == -1) {
		ERROR("Orphaned option %s", option->name);
	}

	if (option->type == OPT_TREE && option->value.tree
	    && !list_empty(*option->value.tree)) {
		if (!recursive) {
			if (option->flags & OPT_AUTOCREATE) {
				recursive = 1;
			} else {
				ERROR("Orphaned unregistered "
					"option in subtree %s!",
					option->name);
				recursive = -1;
			}
		}
		free_options_tree(option->value.tree, recursive);
	}

	done_option(option);
}

/*! @relates option */
void
mark_option_as_deleted(struct option *option)
{
	if (option->type == OPT_TREE) {
		struct option *unmarked;

		assert(option->value.tree);

		foreach (unmarked, *option->value.tree)
			mark_option_as_deleted(unmarked);
	}

	option->box_item->visible = 0;

	option->flags |= (OPT_TOUCHED | OPT_DELETED);
}

/*! @relates option */
void
delete_option(struct option *option)
{
	delete_option_do(option, 1);
}

/*! @relates option */
struct option *
copy_option(struct option *template_, int flags)
{
	struct option *option = mem_calloc(1, sizeof(*option));

	if (!option) return NULL;

	option->name = null_or_stracpy(template_->name);
	option->flags = (template_->flags | OPT_ALLOC);
	option->type = template_->type;
	option->min = template_->min;
	option->max = template_->max;
	option->capt = template_->capt;
	option->desc = template_->desc;
	option->change_hook = template_->change_hook;

	if (!(flags & CO_NO_LISTBOX_ITEM))
		option->box_item = init_option_listbox_item(option);
	if (option->box_item) {
		if (template_->box_item) {
			option->box_item->type = template_->box_item->type;
			option->box_item->depth = template_->box_item->depth;
		}
	}

	if (option_types[template_->type].dup) {
		option_types[template_->type].dup(option, template_, flags);
	} else {
		option->value = template_->value;
	}

	return option;
}

/** Return the shadow option in @a shadow_tree of @a option in @a tree.
 * If @a option isn't yet shadowed in @a shadow_tree, shadow it
 * (i.e. create a copy in @a shadow_tree) along with any ancestors
 * that aren't shadowed. */
struct option *
get_option_shadow(struct option *option, struct option *tree,
                  struct option *shadow_tree)
{

	struct option *shadow_option = NULL;

	assert(option);
	assert(tree);
	assert(shadow_tree);

	if (option == tree) {
		shadow_option = shadow_tree;
	} else if (option->root && option->name) {
		struct option *shadow_root;

		shadow_root = get_option_shadow(option->root, tree,
		                                shadow_tree);
		if (!shadow_root) return NULL;

		shadow_option = get_opt_rec_real(shadow_root, option->name);
		if (!shadow_option) {
			shadow_option = copy_option(option,
			                            CO_SHALLOW
			                             | CO_NO_LISTBOX_ITEM);
			if (shadow_option) {
				shadow_option->root = shadow_root;
				/* No need to sort, is there? It isn't shown
				 * in the options manager. -- Miciah */
				add_to_list_end(*shadow_root->value.tree,
				                shadow_option);

				shadow_option->flags |= OPT_TOUCHED;
			}

		}
	}

	return shadow_option;
}


/*! @relates option */
LIST_OF(struct option) *
init_options_tree(void)
{
	LIST_OF(struct option) *ptr = mem_alloc(sizeof(*ptr));

	if (ptr) init_list(*ptr);
	return ptr;
}

/** Some default pre-autocreated options. Doh. */
static inline void
register_autocreated_options(void)
{
	/* TODO: Use table-driven initialization. --jonas */
	get_opt_int("terminal.linux.type", NULL) = TERM_LINUX;
	get_opt_int("terminal.linux.colors", NULL) = COLOR_MODE_16;
	get_opt_bool("terminal.linux.m11_hack", NULL) = 1;
	get_opt_int("terminal.vt100.type", NULL) = TERM_VT100;
	get_opt_int("terminal.vt110.type", NULL) = TERM_VT100;
	get_opt_int("terminal.xterm.type", NULL) = TERM_VT100;
	get_opt_bool("terminal.xterm.underline", NULL) = 1;
	get_opt_int("terminal.xterm-color.type", NULL) = TERM_VT100;
	get_opt_int("terminal.xterm-color.colors", NULL) = COLOR_MODE_16;
	get_opt_bool("terminal.xterm-color.underline", NULL) = 1;
#ifdef CONFIG_88_COLORS
	get_opt_int("terminal.xterm-88color.type", NULL) = TERM_VT100;
	get_opt_int("terminal.xterm-88color.colors", NULL) = COLOR_MODE_88;
	get_opt_bool("terminal.xterm-88color.underline", NULL) = 1;
#endif
	get_opt_int("terminal.rxvt-unicode.type", NULL) = 1;
#ifdef CONFIG_88_COLORS
	get_opt_int("terminal.rxvt-unicode.colors", NULL) = COLOR_MODE_88;
#else
	get_opt_int("terminal.rxvt-unicode.colors", NULL) = COLOR_MODE_16;
#endif
	get_opt_bool("terminal.rxvt-unicode.italic", NULL) = 1;
	get_opt_bool("terminal.rxvt-unicode.underline", NULL) = 1;
#ifdef CONFIG_256_COLORS
	get_opt_int("terminal.xterm-256color.type", NULL) = TERM_VT100;
	get_opt_int("terminal.xterm-256color.colors", NULL) = COLOR_MODE_256;
	get_opt_bool("terminal.xterm-256color.underline", NULL) = 1;
	get_opt_int("terminal.fbterm.type", NULL) = TERM_FBTERM;
	get_opt_int("terminal.fbterm.colors", NULL) = COLOR_MODE_256;
	get_opt_bool("terminal.fbterm.underline", NULL) = 0;
#endif
}

extern union option_info cmdline_options_info[];

#include "config/options.inc"

static int
change_hook_cache(struct session *ses, struct option *current, struct option *changed)
{
	shrink_memory(0);
	return 0;
}

static int
change_hook_connection(struct session *ses, struct option *current, struct option *changed)
{
	register_check_queue();
	return 0;
}

static int
change_hook_html(struct session *ses, struct option *current, struct option *changed)
{
	foreach (ses, sessions) ses->tab->resize = 1;

	return 0;
}

static int
change_hook_insert_mode(struct session *ses, struct option *current, struct option *changed)
{
	update_status();
	return 0;
}

static int
change_hook_active_link(struct session *ses, struct option *current, struct option *changed)
{
	update_cached_document_options(ses);
	return 0;
}

static int
change_hook_terminal(struct session *ses, struct option *current, struct option *changed)
{
	cls_redraw_all_terminals();
	return 0;
}

static int
change_hook_ui(struct session *ses, struct option *current, struct option *changed)
{
	update_status();
	return 0;
}

/** Make option templates visible or invisible in the option manager.
 * This is called once on startup, and then each time the value of the
 * "config.show_template" option is changed.
 *
 * @param tree
 * The option tree whose children should be affected.
 *
 * @param show
 * A set of bits:
 * - The 0x01 bit means templates should be made visible.
 *   If the bit is clear, templates become invisible instead.
 * - The 0x02 bit means @a tree is itself part of a template,
 *   and so all of its children should be affected, regardless
 *   of whether they are templates of their own.
 *
 * Deleted options are never visible.
 *
 * @relates option */
static void
update_visibility(LIST_OF(struct option) *tree, int show)
{
	struct option *opt;

	foreach (opt, *tree) {
		if (opt->flags & OPT_DELETED) continue;

		if (!strcmp(opt->name, "_template_")) {
			if (opt->box_item)
				opt->box_item->visible = (show & 1);

			if (opt->type == OPT_TREE)
				update_visibility(opt->value.tree, show | 2);
		} else {
			if (opt->box_item && (show & 2))
				opt->box_item->visible = (show & 1);

			if (opt->type == OPT_TREE)
				update_visibility(opt->value.tree, show);
		}
	}
}

static int
change_hook_stemplate(struct session *ses, struct option *current, struct option *changed)
{
	update_visibility(config_options->value.tree, changed->value.number);
	return 0;
}

static int
change_hook_language(struct session *ses, struct option *current, struct option *changed)
{
#ifdef CONFIG_NLS
	set_language(changed->value.number);
#endif
	return 0;
}

static const struct change_hook_info change_hooks[] = {
	{ "config.show_template",	change_hook_stemplate },
	{ "connection",			change_hook_connection },
	{ "document.browse",		change_hook_html },
	{ "document.browse.forms.insert_mode",
					change_hook_insert_mode },
	{ "document.browse.links.active_link",
					change_hook_active_link },
	{ "document.cache",		change_hook_cache },
	{ "document.codepage",		change_hook_html },
	{ "document.colors",		change_hook_html },
	{ "document.html",		change_hook_html },
	{ "document.plain",		change_hook_html },
	{ "terminal",			change_hook_terminal },
	{ "ui.language",		change_hook_language },
	{ "ui",				change_hook_ui },
	{ NULL,				NULL },
};

void
init_options(void)
{
	cmdline_options = add_opt_tree_tree(&options_root, "", "",
					    "cmdline", 0, "");
	register_options(cmdline_options_info, cmdline_options);

	config_options = add_opt_tree_tree(&options_root, "", "",
					 "config", OPT_SORT, "");
	config_options->flags |= OPT_LISTBOX;
	config_options->box_item = &option_browser.root;
	register_options(config_options_info, config_options);

	register_autocreated_options();
	register_change_hooks(change_hooks);
}

/*! @relates option */
static void
free_options_tree(LIST_OF(struct option) *tree, int recursive)
{
	while (!list_empty(*tree))
		delete_option_do(tree->next, recursive);
}

void
done_options(void)
{
	done_domain_trees();
	unregister_options(config_options_info, config_options);
	unregister_options(cmdline_options_info, cmdline_options);
	config_options->box_item = NULL;
	free_options_tree(&options_root_tree, 0);
}

/*! @relates change_hook_info */
void
register_change_hooks(const struct change_hook_info *change_hooks)
{
	int i;

	for (i = 0; change_hooks[i].name; i++) {
		struct option *option = get_opt_rec(config_options,
						    change_hooks[i].name);

		assert(option);
		option->change_hook = change_hooks[i].change_hook;
	}
}

/** Set or clear the ::OPT_MUST_SAVE flag in all descendants of @a tree.
 *
 * @param tree
 * The option tree in which this function recursively sets or clears
 * the flag.
 *
 * @param set_all
 * If true, set ::OPT_MUST_SAVE in all options of the tree.
 * If false, set it only in touched or deleted options, and clear in others.
 *
 * @relates option */
void
prepare_mustsave_flags(LIST_OF(struct option) *tree, int set_all)
{
	struct option *option;

	foreach (option, *tree) {
		/* XXX: OPT_LANGUAGE shouldn't have any bussiness
		 * here, but we're just weird in that area. */
		if (set_all
		    || (option->flags & (OPT_TOUCHED | OPT_DELETED))
		    || option->type == OPT_LANGUAGE)
			option->flags |= OPT_MUST_SAVE;
		else
			option->flags &= ~OPT_MUST_SAVE;

		if (option->type == OPT_TREE)
			prepare_mustsave_flags(option->value.tree, set_all);
	}
}

/** Clear the ::OPT_TOUCHED flag in all descendants of @a tree.
 * @relates option */
void
untouch_options(LIST_OF(struct option) *tree)
{
	struct option *option;

	foreach (option, *tree) {
		option->flags &= ~OPT_TOUCHED;

		if (option->type == OPT_TREE)
			untouch_options(option->value.tree);
	}
}

/*! @relates option */
static int
check_nonempty_tree(LIST_OF(struct option) *options)
{
	struct option *opt;

	foreach (opt, *options) {
		if (opt->type == OPT_TREE) {
			if (check_nonempty_tree(opt->value.tree))
				return 1;
		} else if (opt->flags & OPT_MUST_SAVE) {
			return 1;
		}
	}

	return 0;
}

/*! @relates option */
void
smart_config_string(struct string *str, int print_comment, int i18n,
		    LIST_OF(struct option) *options,
		    unsigned char *path, int depth,
		    void (*fn)(struct string *, struct option *,
			       unsigned char *, int, int, int, int))
{
	struct option *option;

	foreach (option, *options) {
		int do_print_comment = 1;

		if (option->flags & OPT_HIDDEN ||
		    option->type == OPT_ALIAS ||
		    !strcmp(option->name, "_template_"))
			continue;

		/* Is there anything to be printed anyway? */
		if (option->type == OPT_TREE
		    ? !check_nonempty_tree(option->value.tree)
		    : !(option->flags & OPT_MUST_SAVE))
			continue;

		/* We won't pop out the description when we're in autocreate
		 * category and not template. It'd be boring flood of
		 * repetitive comments otherwise ;). */

		/* This print_comment parameter is weird. If it is negative, it
		 * means that we shouldn't print comments at all. If it is 1,
		 * we shouldn't print comment UNLESS the option is _template_
		 * or not-an-autocreating-tree (it is set for the first-level
		 * autocreation tree). When it is 2, we can print out comments
		 * normally. */
		/* It is still broken somehow, as it didn't work for terminal.*
		 * (the first autocreated level) by the time I wrote this. Good
		 * summer job for bored mad hackers with spare boolean mental
		 * power. I have better things to think about, personally.
		 * Maybe we should just mark autocreated options somehow ;). */
		if (!print_comment || (print_comment == 1
					&& (strcmp(option->name, "_template_")
					    && (option->flags & OPT_AUTOCREATE
					        && option->type == OPT_TREE))))
			do_print_comment = 0;

		/* Pop out the comment */

		/* For config file, we ignore do_print_comment everywhere
		 * except 1, but sometimes we want to skip the option totally.
		 */
		fn(str, option, path, depth,
		   option->type == OPT_TREE ? print_comment
					    : do_print_comment,
		   0, i18n);

		fn(str, option, path, depth, do_print_comment, 1, i18n);

		/* And the option itself */

		if (option_types[option->type].write) {
			fn(str, option, path, depth,
			   do_print_comment, 2, i18n);

		} else if (option->type == OPT_TREE) {
			struct string newpath;
			int pc = print_comment;

			if (!init_string(&newpath)) continue; /* OK? */

			if (pc == 2 && option->flags & OPT_AUTOCREATE)
				pc = 1;
			else if (pc == 1 && strcmp(option->name, "_template_"))
				pc = 0;

			fn(str, option, path, depth, /*pc*/1, 3, i18n);

			if (path) {
				add_to_string(&newpath, path);
				add_char_to_string(&newpath, '.');
			}
			add_to_string(&newpath, option->name);
			smart_config_string(str, pc, i18n, option->value.tree,
					    newpath.source, depth + 1, fn);
			done_string(&newpath);

			fn(str, option, path, depth, /*pc*/1, 3, i18n);
		}
	}
}


void
update_options_visibility(void)
{
	update_visibility(config_options->value.tree,
			  get_opt_bool("config.show_template", NULL));
}

void
toggle_option(struct session *ses, struct option *option)
{
	long number = option->value.number + 1;

	assert(option->type == OPT_BOOL || option->type == OPT_INT);
	assert(option->max);

	option->value.number = (number <= option->max) ? number : option->min;
	option_changed(ses, option);
}



void
call_change_hooks(struct session *ses, struct option *current, struct option *option)
{
	/* This boolean thing can look a little weird - it
	 * basically says that we should proceed when there's
	 * no change_hook or there's one and its return value
	 * was zero. */
	while (current && (!current->change_hook ||
		!current->change_hook(ses, current, option))) {
		if (!current->root)
			break;

		current = current->root;
	}
}

void
option_changed(struct session *ses, struct option *option)
{
	option->flags |= OPT_TOUCHED;
	/* Notify everyone out there! */
	call_change_hooks(ses, option, option);
}

/*! @relates option_resolver */
int
commit_option_values(struct option_resolver *resolvers,
		     struct option *root, union option_value *values, int size)
{
	int touched = 0;
	int i;

	assert(resolvers && root && values && size);

	for (i = 0; i < size; i++) {
		unsigned char *name = resolvers[i].name;
		struct option *option = get_opt_rec(root, name);
		int id = resolvers[i].id;

		assertm(option, "Bad option '%s' in options resolver", name);

		if (memcmp(&option->value, &values[id], sizeof(union option_value))) {
			option->value = values[id];
			option->flags |= OPT_TOUCHED;
			/* Speed hack: Directly call the change-hook for each
			 * option in resolvers and later call call_change_hooks
			 * on the root; if we were to call call_change_hooks
			 * on each option in resolvers, we would end up calling
			 * the change-hooks of root, its parents, its
			 * grandparents, and so on for each option in resolvers
			 * because call_change_hooks is recursive. -- Miciah */
			if (option->change_hook)
				option->change_hook(NULL, option, NULL);
			touched++;
		}
	}

	/* See above 'Speed hack' comment. */
	call_change_hooks(NULL, root, NULL);

	return touched;
}

/*! @relates option_resolver */
void
checkout_option_values(struct option_resolver *resolvers,
		       struct option *root,
		       union option_value *values, int size)
{
	int i;

	for (i = 0; i < size; i++) {
		unsigned char *name = resolvers[i].name;
		struct option *option = get_opt_rec(root, name);
		int id = resolvers[i].id;

		assertm(option, "Bad option '%s' in options resolver", name);

		values[id] = option->value;
	}
}

/**********************************************************************
 Options values
**********************************************************************/


/*! @relates option_info */
void
register_options(union option_info info[], struct option *tree)
{
	int i;
	static const struct option zero = INIT_OPTION(
		NULL, 0, 0, 0, 0, 0, NULL, NULL);

	/* To let unregister_options() correctly find the end of the
	 * info[] array, this loop must convert every element from
	 * struct option_init to struct option.  That is, even if
	 * there is not enough memory to fully initialize some option,
	 * the loop must continue.  */
	for (i = 0; info[i].init.path; i++) {
		/* Copy the value aside before it is overwritten
		 * with the other member of the union.  */
		const struct option_init init = info[i].init;

		struct option *option = &info[i].option;
		unsigned char *string;

		*option = zero;
		option->name = init.name;
		option->capt = init.capt;
		option->desc = init.desc;
		option->flags = init.flags;
		option->type = init.type;
		option->min = init.min;
		option->max = init.max;
		/* init.value_long, init.value_dataptr, or init.value_funcptr
		 * is handled below as appropriate for each type.  */

		debug_check_option_syntax(option);

		if (option->type != OPT_ALIAS
		    && ((tree->flags & OPT_LISTBOX)
			|| (option->flags & OPT_LISTBOX))) {
			option->box_item = init_option_listbox_item(option);
			if (!option->box_item) {
				delete_option(option);
				continue;
			}
		}

		switch (option->type) {
			case OPT_TREE:
				option->value.tree = init_options_tree();
				if (!option->value.tree) {
					delete_option(option);
					continue;
				}
				break;
			case OPT_STRING:
				string = mem_alloc(MAX_STR_LEN);
				if (!string) {
					delete_option(option);
					continue;
				}
				safe_strncpy(string, init.value_dataptr, MAX_STR_LEN);
				option->value.string = string;
				break;
			case OPT_COLOR:
				string = init.value_dataptr;
				assert(string);
				decode_color(string, strlen(string),
						&option->value.color);
				break;
			case OPT_CODEPAGE:
				string = init.value_dataptr;
				assert(string);
				option->value.number = get_cp_index(string);
				break;
			case OPT_BOOL:
			case OPT_INT:
				option->value.number = init.value_long;
				break;
			case OPT_LONG:
				option->value.big_number = init.value_long;
				break;
			case OPT_LANGUAGE:
				/* INIT_OPT_LANGUAGE has no def parameter */
				option->value.number = 0;
				break;
			case OPT_COMMAND:
				option->value.command = init.value_funcptr;
				break;
			case OPT_ALIAS:
				option->value.string = init.value_dataptr;
				break;
		}

		add_opt_rec(tree, init.path, option);
	}

	/* Convert the sentinel at the end of the array, too.  */
	info[i].option = zero;
}

/*! @relates option_info */
void
unregister_options(union option_info info[], struct option *tree)
{
	int i = 0;

	/* We need to remove the options in inverse order to the order how we
	 * added them. */

	while (info[i].option.name) i++;

	for (i--; i >= 0; i--)
		delete_option_do(&info[i].option, 0);
}
