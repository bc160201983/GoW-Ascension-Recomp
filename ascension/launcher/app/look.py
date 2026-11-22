"""The "Clean" look (2026-11): the window's frame and sidebar, the Home and
Graphics pages, search, automatic saving, and the helpers every settings page
is built with (cards, rows, switches). What the pages do is in ui.py (Launcher,
which takes these methods from Look)."""

import ctypes
import shutil
import threading
import time
import webbrowser

from .config import (ANISO_LEVELS, ASPECTS, CONFIG_FILE, DISPLAY_MODES, FPS_CAPS, FPS_LIMIT, FSR_MODES, GAME_ID,
    GAME_WINDOWS, MAC, NEWS, ON_OFF, PRESETS, PRESET_NAMES, PRESET_NOTES, PRESET_ORDER, RENDERERS, RENDER_SCALES,
    ROOT, SCALE_FILTERS, SHARPEN_LEVELS, SHORT_LABELS, VERSION, VIBRANCE_LEVELS, VSYNC_MODES, WINDOWS, WINDOW_SIZES,
    fps_limit, fps_unlocked, save_settings)
from .game import disc_info, missing, prebuild_mods, save_dir
from .i18n import tr
from . import updater
from . import widgets as W

# The names the pages use for their own labels and tables. PANEL is what
# controls sit on (a card), CARD a thing raised on it (a key cell, a list).
BG, PANEL, CARD, LINE = W.PAGE, W.CARD, W.RAISED, W.LINE
TEXT, MUTED, ACCENT, ACCENT_HI = W.TEXT, W.MUTED, '#b8872f', W.GOLD
GOLD, LOG_BG, LOG_FG, OFF = W.GOLD, W.FIELD, '#cfc6bc', W.DIM

PAGES = ('home', 'graphics', 'game', 'controls', 'patches', 'mods', 'trophies', 'online', 'advanced', 'support',
         'about')


def translated(options):
    """[(value, label)] with the labels in the launcher's language."""
    return [(value, tr(label)) for value, label in options]


def brief(hint, limit=118):
    """The first sentence of an explanation: what a row shows under its name
    (the whole text is in the row's tooltip)."""
    if not hint:
        return ''
    text = ' '.join(str(hint).split())
    if len(text) <= limit:
        return text
    cut = text.find('. ')
    first = text if cut < 0 else text[:cut + 1]
    if len(first) > limit:
        first = first[:limit].rsplit(' ', 1)[0] + '…'
    return first


def pad_connected():
    """Whether an XInput controller is on (Windows); None when it cannot be told."""
    if not WINDOWS:
        return None
    for name in ('xinput1_4', 'xinput1_3', 'xinput9_1_0'):
        try:
            dll = ctypes.WinDLL(name)
        except OSError:
            continue
        try:
            state = ctypes.create_string_buffer(16)          # XINPUT_STATE
            return any(dll.XInputGetState(i, state) == 0 for i in range(4))
        except (AttributeError, OSError, ValueError):
            return None
    return None


def last_save():
    """When the newest save file was written, or None."""
    newest = 0.0
    try:
        for folder in save_dir().iterdir():
            if folder.is_dir():
                for f in folder.iterdir():
                    newest = max(newest, f.stat().st_mtime)
    except OSError:
        pass
    return newest or None


class HeroPlay:
    """The big PLAY on Home's picture: canvas items, so the art shows around its round ends."""

    def __init__(self, app, canvas, command):
        self.app, self.canvas, self.command = app, canvas, command
        self.text, self.disabled, self.hot, self.xy = tr('PLAY'), False, False, None
        canvas.tag_bind('heroplay', '<Button-1>', lambda _e: not self.disabled and self.command())
        canvas.tag_bind('heroplay', '<Enter>', lambda _e: self.hover(True))
        canvas.tag_bind('heroplay', '<Leave>', lambda _e: self.hover(False))

    def size(self):
        return self.app.px(216), self.app.px(56)

    def hover(self, on):
        # draw() replaces the items under the pointer, and Tk then sends <Enter> again: without
        # this check a pointer resting on PLAY redrew it ~1,500 times a second, forever (a hang).
        if on == self.hot:
            return
        self.hot = on
        self.canvas.configure(cursor='hand2' if on and not self.disabled else '')
        self.draw()

    def place(self, x, y):
        self.xy = (x, y)
        self.draw()

    def draw(self):
        c, app = self.canvas, self.app
        c.delete('heroplay')
        if self.xy is None:
            return
        (x, y), (w, h) = self.xy, self.size()
        top, low = (W.RED_HI, W.RED_DK) if not self.hot else (W.mix(W.RED_HI, '#ffffff', 0.10), W.mix(W.RED_DK, '#ffffff', 0.08))
        if self.disabled:
            top = low = '#3a2426'
        fg = '#ffffff' if not self.disabled else '#a08e8c'
        image = W.shape(w, h, h // 2, top, None, fill2=low)
        if image is not None:
            self.image = W.photo(image)
            c.create_image(x, y, anchor='nw', image=self.image, tags='heroplay')
        else:
            c.create_rectangle(x, y, x + w, y + h, fill=W.RED, outline='', tags='heroplay')
        label = c.create_text(0, 0, text=self.text, fill=fg, font=(app.heading, 16), anchor='w', tags='heroplay')
        lw = c.bbox(label)[2] - c.bbox(label)[0]
        if app.icons and not self.disabled:
            mark = c.create_text(0, 0, text=W.ICONS['play'], fill=fg, font=(app.icons, 12), anchor='w', tags='heroplay')
            mw, gap = c.bbox(mark)[2] - c.bbox(mark)[0], app.px(12)
            left = x + (w - (mw + gap + lw)) // 2
            c.coords(mark, left, y + h // 2 + 1)
            c.coords(label, left + mw + gap, y + h // 2)
        else:
            c.coords(label, x + (w - lw) // 2, y + h // 2)

    def configure(self, text=None, state=None):
        if text is not None:
            self.text = text
        if state is not None:
            self.disabled = str(state) == 'disabled'
        self.draw()

    config = configure


class Look:
    # ---- look -------------------------------------------------------------------------------
    def style(self):
        from tkinter import font as tkfont
        ttk = self.ttk
        families = set(tkfont.families(self.root))
        self.icons = next((f for f in ('Segoe Fluent Icons', 'Segoe MDL2 Assets') if f in families), None)
        self.semi = 'Segoe UI Semibold' if 'Segoe UI Semibold' in families else self.ui
        self.title_font = (self.semi, 19) if self.semi != self.ui else (self.ui, 19, 'bold')
        s = ttk.Style(self.root)
        s.theme_use('clam')
        base = (self.ui, 10)
        for widget in ('TEntry', 'TCombobox'):
            self.root.option_add(f'*{widget}.font', base)
        self.root.option_add('*TCombobox*Listbox.background', W.RAISED)
        self.root.option_add('*TCombobox*Listbox.foreground', TEXT)
        self.root.option_add('*TCombobox*Listbox.selectBackground', W.GOLD)
        self.root.option_add('*TCombobox*Listbox.selectForeground', W.INK)
        self.root.option_add('*TCombobox*Listbox.font', base)
        s.configure('.', background=PANEL, foreground=TEXT, fieldbackground=W.FIELD, bordercolor=W.LINE,
                    lightcolor=W.LINE, darkcolor=W.LINE, troughcolor=W.PAGE, focuscolor=PANEL, font=base)
        s.configure('TFrame', background=PANEL)
        s.configure('Card.TFrame', background=CARD)
        s.configure('TLabel', background=PANEL, foreground=TEXT)
        s.configure('Muted.TLabel', background=PANEL, foreground=MUTED, font=(self.ui, 9))
        s.configure('Off.TLabel', background=PANEL, foreground=OFF)
        s.configure('Badge.TLabel', background=PANEL, foreground=GOLD, font=(self.ui, 8, 'bold'))
        s.configure('Gold.TLabel', background=PANEL, foreground=GOLD, font=(self.ui, 8, 'bold'))
        s.configure('Section.TLabel', background=PANEL, foreground=MUTED, font=(self.semi, 10))
        s.configure('Value.TLabel', background=PANEL, foreground=TEXT, font=(self.ui, 10, 'bold'))
        s.configure('Warning.TLabel', background='#2a1414', foreground='#e0a0a0', padding=(10, 6))
        s.configure('TCombobox', arrowcolor=MUTED, foreground=TEXT, padding=5, arrowsize=14, bordercolor=W.LINE,
                    lightcolor=W.FIELD, darkcolor=W.FIELD, background=W.FIELD)
        s.map('TCombobox', fieldbackground=[('readonly', W.FIELD)], foreground=[('readonly', TEXT)],
              selectbackground=[('readonly', W.FIELD)], selectforeground=[('readonly', TEXT)],
              background=[('active', W.RAISED)])
        s.configure('TEntry', foreground=TEXT, insertcolor=TEXT, padding=5, bordercolor=W.LINE, lightcolor=W.FIELD,
                    darkcolor=W.FIELD)
        s.configure('TButton', background=W.RAISED, foreground=TEXT, padding=(13, 7), borderwidth=0,
                    bordercolor=W.RAISED, lightcolor=W.RAISED, darkcolor=W.RAISED, focuscolor=W.RAISED)
        s.map('TButton', background=[('active', '#2c2c32'), ('disabled', PANEL)], foreground=[('disabled', OFF)],
              lightcolor=[('active', '#2c2c32')], darkcolor=[('active', '#2c2c32')],
              bordercolor=[('active', '#2c2c32')])
        s.configure('Accent.TButton', background=W.GOLD, foreground=W.INK, borderwidth=0, bordercolor=W.GOLD,
                    lightcolor=W.GOLD, darkcolor=W.GOLD, focuscolor=W.GOLD, font=(self.ui, 10, 'bold'))
        s.map('Accent.TButton', background=[('active', W.GOLD_HI), ('disabled', '#3a3220')],
              foreground=[('disabled', '#8a7f68')], lightcolor=[('active', W.GOLD_HI)],
              darkcolor=[('active', W.GOLD_HI)], bordercolor=[('active', W.GOLD_HI)])
        s.configure('Small.TButton', padding=(5, 2))
        s.configure('Link.TButton', background=PANEL, foreground=GOLD, borderwidth=0, padding=(2, 1),
                    font=(self.ui, 9), bordercolor=PANEL, lightcolor=PANEL, darkcolor=PANEL, focuscolor=PANEL)
        s.map('Link.TButton', background=[('active', PANEL)], foreground=[('active', W.GOLD_HI)],
              lightcolor=[('active', PANEL)], darkcolor=[('active', PANEL)], bordercolor=[('active', PANEL)])
        s.configure('Horizontal.TProgressbar', background=W.GOLD, troughcolor=W.FIELD, bordercolor=W.LINE,
                    lightcolor=W.GOLD_HI, darkcolor=W.GOLD)
        for name in ('TScrollbar', 'Vertical.TScrollbar'):
            s.configure(name, background='#2c2c31', arrowcolor=W.PAGE, troughcolor=W.PAGE, bordercolor=W.PAGE,
                        lightcolor='#2c2c31', darkcolor='#2c2c31', gripcount=0, arrowsize=self.px(11))
            s.map(name, background=[('active', '#3a3a40')], lightcolor=[('active', '#3a3a40')],
                  darkcolor=[('active', '#3a3a40')])

    # ---- settings widgets -------------------------------------------------------------------
    def body(self, parent):
        """Where a page's next thing goes: the card it opened last, else the page itself."""
        return getattr(parent, 'card_body', None) or parent

    @staticmethod
    def next_row(parent):
        return (getattr(parent, 'card_body', None) or parent).grid_size()[1]

    def put(self, parent, widget, **grid):
        """Grid a widget made on the page into the page's current card (a new row
        across it unless told otherwise)."""
        b = self.body(parent)
        grid.setdefault('row', b.grid_size()[1])
        grid.setdefault('column', 0)
        grid.setdefault('columnspan', 2)
        inside = widget.master is not b
        if inside:
            grid['in_'] = b
        widget.grid(**grid)
        if inside:
            widget.tk.call('raise', widget._w)
        return widget

    def card(self, parent):
        """A new rounded panel on a page; rows, notes and buttons that follow go into it."""
        c = W.Card(self, parent)
        n = parent.grid_size()[1]
        c.grid(row=n, column=0, columnspan=2, sticky='we', pady=(0 if n == 0 else self.px(14), 0))
        c.body.columnconfigure(0, weight=1)
        parent.card_body = c.body
        return c

    def section(self, parent, title, top=22):
        c = self.card(parent)
        if title:
            self.tk.Label(c.body, text=title, bg=PANEL, fg=MUTED, font=(self.semi, 10), anchor='w').grid(
                row=0, column=0, columnspan=2, sticky='w', pady=(0, self.px(4)))
        return c

    def segments(self, key, options):
        """The short labels a choice shows side by side, or None when it is a
        drop-down (many options, or long ones)."""
        short = SHORT_LABELS.get(key)
        values = [value for value, _label in options]
        if short:
            pairs = [(value, tr(label)) for value, label in short if value in values]
            if len(pairs) != len(values):
                pairs = None
        else:
            pairs = translated(options)
        if not pairs or len(pairs) > 5 or sum(len(label) for _v, label in pairs) > 42:
            return None
        return pairs

    def choice(self, parent, key, options, width=28):
        """A setting with a few named values: a switch for On/Off, the values side
        by side when they are few and short, else a read-only drop-down. `options`
        may be a function, re-read by refresh_choices when what it depends on changes."""
        source = options
        if not callable(source):
            if list(source) == list(ON_OFF):
                return W.Toggle(self, parent, self.var(key), bg=parent.cget('bg') if isinstance(parent, self.tk.Frame) else PANEL)
            pairs = self.segments(key, source)
            if pairs:
                return W.Segmented(self, parent, self.var(key), pairs, bg=PANEL)
        options = translated(source() if callable(source) else source)
        shown = self.tk.StringVar()
        box = self.ttk.Combobox(parent, textvariable=shown, state='readonly', width=width)
        box.maps = (dict(options), {label: value for value, label in options})
        box.configure(values=[label for _, label in options])
        var = self.var(key)
        shown.set(box.maps[0].get(var.get(), options[0][1]))
        shown.trace_add('write', lambda *_: shown.get() in box.maps[1] and var.get() != box.maps[1][shown.get()]
                        and var.set(box.maps[1][shown.get()]))
        # the same setting shown elsewhere (Home and its page) changes this one too
        var.trace_add('write', lambda *_: var.get() in box.maps[0] and shown.get() != box.maps[0][var.get()]
                      and shown.set(box.maps[0][var.get()]))
        box.bind('<MouseWheel>', lambda e: 'break')
        if callable(source):
            self.dynamic.setdefault(key, []).append((box, shown, source))
        return box

    def row(self, parent, title, widget, hint=None, badge=None):
        """A setting: its name and the first sentence of its explanation on the
        left (the whole explanation when the mouse rests on it), the control on the
        right, or under them when it is wide."""
        tk, px = self.tk, self.px
        b = self.body(parent)
        r = b.grid_size()[1]
        if getattr(b, 'has_rows', False):
            tk.Frame(b, bg=W.LINE, height=1).grid(row=r, column=0, columnspan=2, sticky='we', pady=(px(9), px(9)))
            r += 1
        b.has_rows = True
        cell = tk.Frame(b, bg=PANEL)
        line = tk.Frame(cell, bg=PANEL)
        line.pack(anchor='w')
        name = tk.Label(line, text=title, bg=PANEL, fg=TEXT, font=(self.ui, 11))
        name.pack(side='left')
        if badge:
            tk.Label(line, text=f' {badge} ', bg=GOLD, fg=W.INK, font=(self.ui, 7, 'bold')).pack(
                side='left', padx=(px(8), 0))
        short = brief(hint)
        if short:
            note = tk.Label(cell, text=short, bg=PANEL, fg=MUTED, font=(self.ui, 9), justify='left',
                            wraplength=px(430))
            note.pack(anchor='w', pady=(px(1), 0))
            if short != ' '.join(str(hint).split()):
                for w in (name, note):
                    W.Tooltip(self, w, hint)
        cell.title = name
        widget.update_idletasks()
        wide = widget.winfo_reqwidth() > px(390)
        cell.grid(row=r, column=0, columnspan=2 if wide else 1, sticky='w')
        inside = {} if widget.master is b else {'in_': b}
        if wide:
            widget.grid(row=r + 1, column=0, columnspan=2, sticky='w', pady=(px(7), 0), **inside)
        else:
            widget.grid(row=r, column=1, sticky='e', padx=(px(18), 0), **inside)
        if inside:
            widget.tk.call('raise', widget._w)
        if self.building and getattr(self, 'indexing', True):
            self.index.append({'title': title, 'hint': hint or '', 'page': self.building, 'cell': cell,
                               'text': f'{title} {hint or ""}'.lower()})
        return widget

    def check(self, parent, key, title, hint=None, badge=None, command=None):
        return self.row(parent, title, W.Toggle(self, parent, self.var(key), command=command, bg=PANEL), hint, badge)

    def note(self, parent, text, top=10, style='Muted.TLabel'):
        b = self.body(parent)
        label = self.ttk.Label(b, text=text, style=style, wraplength=self.px(660), justify='left')
        label.grid(row=b.grid_size()[1], column=0, columnspan=2, sticky='w', pady=(min(top, self.px(10)), 0))
        return label

    def unavailable(self, parent, items, why):
        """Settings this build does not have yet: listed, greyed, with no control."""
        b = self.body(parent)
        r = b.grid_size()[1]
        for i, (title, what) in enumerate(items):
            self.ttk.Label(b, text=title, style='Off.TLabel').grid(row=r + i, column=0, sticky='w', pady=(4, 0),
                                                                   padx=(0, self.px(22)))
            self.ttk.Label(b, text=what, style='Off.TLabel').grid(row=r + i, column=1, sticky='e', pady=(4, 0))
        self.note(parent, why, top=8)

    def apply_preset(self, name):
        for key, value in PRESETS[name].items():
            self.var(key).set(value)
        self.refresh_home()

    def buttons(self, parent, items, top=8):
        b = self.body(parent)
        holder = self.ttk.Frame(b)
        holder.grid(row=b.grid_size()[1], column=0, columnspan=2, sticky='w', pady=(max(top, self.px(10)), 0))
        out = []
        for i, (text, command) in enumerate(items):
            button = self.ttk.Button(holder, text=text, command=command)
            button.pack(side='left', padx=(0 if i == 0 else 8, 0))
            out.append(button)
        return holder, out

    def path_row(self, f, key, title, folder=False, hint=None):
        from pathlib import Path
        holder = self.ttk.Frame(f)
        self.ttk.Entry(holder, textvariable=self.var(key), width=62).pack(side='left')

        def browse():
            current = self.var(key).get()
            if folder:
                p = self.filedialog.askdirectory(initialdir=current or str(ROOT))
            else:
                p = self.filedialog.askopenfilename(initialdir=str(Path(current).parent) if current else str(ROOT))
            if p:
                self.var(key).set(str(Path(p)))
        self.ttk.Button(holder, text=tr('Browse…'), command=browse).pack(side='left', padx=(8, 0))
        self.row(f, title, holder, hint)

    def log_box(self, parent, height=14):
        b = self.body(parent)
        holder = self.ttk.Frame(b)
        holder.grid(row=b.grid_size()[1], column=0, columnspan=2, sticky='we', pady=(self.px(8), 0))
        text = self.tk.Text(holder, wrap='none', height=height, bg=LOG_BG, fg=LOG_FG, insertbackground=TEXT,
                            relief='flat', font=(self.mono, 9), padx=10, pady=8, state='disabled',
                            highlightthickness=0)
        bar = self.ttk.Scrollbar(holder, command=text.yview)
        text.configure(yscrollcommand=bar.set)
        bar.pack(side='right', fill='y')
        text.pack(side='left', fill='both', expand=True)
        return text

    def search_box(self, parent, page):
        """The search field of a page's heading: all of them show the same text."""
        tk, px = self.tk, self.px
        box = W.Card(self, parent, fill=W.CARD, back=W.PAGE, radius=9, padx=12, pady=7)
        if self.icons:
            tk.Label(box.body, text=W.ICONS['search'], bg=W.CARD, fg=W.DIM, font=(self.icons, 10)).pack(
                side='left', padx=(0, px(8)))
        entry = tk.Entry(box.body, textvariable=self.search_var, bg=W.CARD, fg=TEXT, insertbackground=TEXT,
                         relief='flat', bd=0, highlightthickness=0, width=22, font=(self.ui, 10))
        entry.pack(side='left')
        ghost = tk.Label(entry, text=tr('Search settings'), bg=W.CARD, fg=W.DIM, font=(self.ui, 10), cursor='xterm')
        ghost.bind('<Button-1>', lambda _e: entry.focus_set())

        def show_ghost(*_):
            if self.search_var.get() or entry.focus_get() is entry:
                ghost.place_forget()
            else:
                ghost.place(x=0, y=0, relheight=1)
        entry.bind('<FocusIn>', show_ghost, add='+')
        entry.bind('<FocusOut>', show_ghost, add='+')
        entry.bind('<Escape>', lambda _e: self.search_var.set(''))
        self.search_var.trace_add('write', lambda *_: entry.winfo_exists() and show_ghost())
        show_ghost()
        if page == 'search':
            self.search_entry = entry
        return box

    def scrolled_page(self, name, title, subtitle):
        """A settings page: a heading (its name, the search field), then cards that scroll."""
        tk, px = self.tk, self.px
        outer = tk.Frame(self.content, bg=W.PAGE)
        head = tk.Frame(outer, bg=W.PAGE)
        head.pack(fill='x', padx=(px(20), px(30)), pady=(px(22), px(12)))
        label = tk.Label(head, text=title, bg=W.PAGE, fg='#ffffff', font=self.title_font)
        label.pack(side='left')
        if subtitle:
            W.Tooltip(self, label, subtitle)
        self.search_box(head, name).pack(side='right')
        area = tk.Frame(outer, bg=W.PAGE)
        area.pack(fill='both', expand=True)
        area.rowconfigure(0, weight=1)
        area.columnconfigure(0, weight=1)
        canvas = tk.Canvas(area, bg=W.PAGE, highlightthickness=0, bd=0, yscrollincrement=1)   # scroll in pixels
        bar = self.ttk.Scrollbar(area, orient='vertical', command=canvas.yview)
        inner = tk.Frame(canvas, bg=W.PAGE)
        inner.columnconfigure(0, weight=1)
        left, right, bottom = px(20), px(30), px(26)
        window = canvas.create_window(left, 0, window=inner, anchor='nw')

        def fit(_event=None):
            need = inner.winfo_reqheight() + bottom
            canvas.configure(scrollregion=(0, 0, max(1, canvas.winfo_width()), need))
            if need > canvas.winfo_height() > 1:
                bar.grid(row=0, column=1, sticky='ns')
            else:
                bar.grid_remove()
                canvas.yview_moveto(0)
        inner.bind('<Configure>', fit)
        canvas.bind('<Configure>', lambda e: (canvas.itemconfigure(window, width=max(60, e.width - left - right)), fit()))
        canvas.configure(yscrollcommand=bar.set)
        canvas.grid(row=0, column=0, sticky='nsew')
        outer.scroll = lambda n: (canvas.yview_scroll(int(n), 'units')
                                  if inner.winfo_reqheight() + bottom > canvas.winfo_height() else None)
        outer.head, outer.canvas, outer.inner = head, canvas, inner
        self.pages[name] = outer
        self.building = name
        return inner

    # ---- layout -----------------------------------------------------------------------------
    def build(self):
        tk, px = self.tk, self.px
        side = self.side = tk.Frame(self.root, bg=W.PAGE, width=px(216))
        side.pack(side='left', fill='y')
        side.pack_propagate(False)
        self.side_logo = tk.Label(side, bg=W.PAGE, bd=0)
        self.side_logo.pack(anchor='w', padx=px(24), pady=(px(26), px(22)))
        self.draw_side_logo()
        self.nav, self.current = {}, None
        self.titles = {'home': tr('Home'), 'graphics': tr('Graphics'), 'game': tr('Game'), 'controls': tr('Controls'),
                       'patches': tr('Patches'), 'mods': tr('Mods'), 'trophies': tr('Trophies'), 'online': tr('Online'), 'advanced': tr('Advanced'),
                       'support': tr('Support'), 'about': tr('About'), 'search': tr('Search')}
        for name in PAGES:
            if name == 'support':          # the gold button under the list
                continue
            item = W.NavItem(self, side, name, self.titles[name], self.show, px(188))
            item.pack(padx=px(14), pady=(0, px(3)), anchor='w')
            self.nav[name] = item
        tk.Frame(side, bg=W.PAGE).pack(fill='both', expand=True)
        self.music_label = tk.Label(side, text='', bg=W.PAGE, fg=W.DIM, font=(self.ui, 9), cursor='hand2')
        self.music_label.pack(anchor='w', padx=px(28), pady=(0, px(12)))
        self.music_label.bind('<Button-1>', lambda _e: self.toggle_music())
        self.refresh_music_label()
        W.PillButton(self, side, tr('Support the port'), lambda: self.show('support'), 188, 42, bg=W.PAGE,
                     kind='gold', icon='support').pack(padx=px(14), pady=(0, px(12)), anchor='w')
        foot = tk.Frame(side, bg=W.PAGE)
        foot.pack(anchor='w', padx=px(28), pady=(0, px(18)))
        dot = tk.Canvas(foot, width=px(9), height=px(9), bg=W.PAGE, bd=0, highlightthickness=0)
        dot.create_oval(0, 0, px(8), px(8), fill=W.GREEN, outline='')
        dot.pack(side='left', padx=(0, px(8)))
        self.version_label = tk.Label(foot, text=f'v{VERSION}', bg=W.PAGE, fg=W.DIM, font=(self.ui, 9))
        self.version_label.pack(side='left')

        right = self.right = tk.Frame(self.root, bg=W.PAGE)
        right.pack(side='left', fill='both', expand=True)
        bar = self.bar = tk.Frame(right, bg=W.PAGE, height=px(54))
        bar.pack(fill='x', side='bottom')
        bar.pack_propagate(False)
        self.status = tk.Label(bar, text='', bg=W.PAGE, fg=MUTED, font=(self.ui, 10), anchor='w', justify='left')
        self.status.pack(side='left', padx=(px(20), 0))
        self.bar_play = W.PillButton(self, bar, tr('PLAY'), self.play, 128, 38, bg=W.PAGE, kind='play', icon='play',
                                     size=12)
        self.saved_label = tk.Label(bar, text='', bg=W.PAGE, fg='#8fd19c', font=(self.ui, 9))
        self.saved_label.pack(side='right', padx=(0, px(30)))
        self.content = tk.Frame(right, bg=W.PAGE)
        self.content.pack(fill='both', expand=True)

        self.pages, self.index, self.building = {}, [], None
        self.search_var = tk.StringVar()
        self.search_var.trace_add('write', lambda *_: self.root.after_idle(self.search_changed))
        self.build_home()
        self.play_button = W.Several(self.hero_play, self.bar_play)
        self.build_graphics()
        self.build_game()
        self.build_controls()
        self.build_patches()
        self.build_mods()
        self.build_trophies()
        self.build_online()
        self.build_advanced()
        self.build_support()
        self.build_about()
        self.build_search()
        self.building = None
        from .game import language_fallback
        self.refresh_choices('language', language_fallback(self.current_mod_settings()))   # PT-BR only while its mod is on
        self.root.bind_all('<MouseWheel>', self.wheel)
        if self.tk.TkVersion >= 8.7:     # trackpads: their own event, in pixels
            self.root.bind_all('<TouchpadScroll>', self.touchpad)
        if not (WINDOWS or MAC) and self.tk.TkVersion < 8.7:   # X11 before Tk 8.7: the wheel is buttons 4 and 5
            self.root.bind_all('<Button-4>', lambda e: self.wheel(e, -1))
            self.root.bind_all('<Button-5>', lambda e: self.wheel(e, 1))
        self.refresh_status()

    def draw_side_logo(self):
        self.side_logo_image = self.scaled_logo(self.px(150))
        if self.side_logo_image:
            self.side_logo.configure(image=self.side_logo_image, text='')
        else:
            self.side_logo.configure(text='GOD OF WAR\nASCENSION', fg=TEXT, font=(self.heading, 13), justify='left')

    def show(self, name):
        self.cancel_capture()
        if name != 'search' and self.search_var.get():
            self.search_var.set('')
        for p in self.pages.values():
            p.pack_forget()
        self.pages[name].pack(fill='both', expand=True)
        self.current = name
        for n, item in self.nav.items():
            item.select(n == name)
        if name == 'home':
            self.bar_play.pack_forget()
        else:
            self.bar_play.pack(side='right', padx=(0, self.px(30)), before=self.saved_label)
        if name == 'mods':
            self.refresh_mods()
        elif name == 'trophies':
            self.refresh_trophies()
        elif name == 'game':
            self.refresh_backups()
        elif name == 'patches' and self.patch_list is None and not self.patch_error:
            self.load_patches()
        elif name == 'home':
            self.root.after_idle(self.draw_banner)
            self.refresh_home(checks=True)

    # ---- search -----------------------------------------------------------------------------
    def build_search(self):
        self.search_page = self.scrolled_page('search', tr('Search'), '')
        self.building = None

    def search_changed(self):
        query = self.search_var.get().strip()
        if not query:
            if self.current == 'search':
                self.show(getattr(self, 'search_back', None) or 'home')
            return
        if self.current != 'search':
            self.search_back = self.current
        self.fill_search(query)
        if self.current != 'search':
            self.show('search')
            entry = getattr(self, 'search_entry', None)
            if entry is not None:
                entry.focus_set()
                entry.icursor('end')

    def fill_search(self, query):
        tk, px = self.tk, self.px
        f = self.search_page
        for w in f.winfo_children():
            w.destroy()
        f.card_body = None
        words = query.lower().split()
        found = [e for e in self.index
                 if all(w in e['text'] or w in self.titles.get(e['page'], '').lower() for w in words)][:40]
        c = self.card(f)
        if not found:
            tk.Label(c.body, text=tr('Nothing found for "{query}".', query=query), bg=PANEL, fg=MUTED,
                     font=(self.ui, 10)).grid(row=0, column=0, sticky='w')
            return
        for i, entry in enumerate(found):
            if i:
                tk.Frame(c.body, bg=W.LINE, height=1).grid(row=2 * i - 1, column=0, sticky='we', pady=(px(8), px(8)))
            line = tk.Frame(c.body, bg=PANEL, cursor='hand2')
            line.grid(row=2 * i, column=0, sticky='we')
            top = tk.Frame(line, bg=PANEL, cursor='hand2')
            top.pack(anchor='w')
            a = tk.Label(top, text=entry['title'], bg=PANEL, fg=TEXT, font=(self.ui, 11), cursor='hand2')
            a.pack(side='left')
            b = tk.Label(top, text='  ·  ' + self.titles.get(entry['page'], ''), bg=PANEL, fg=GOLD,
                         font=(self.ui, 9), cursor='hand2')
            b.pack(side='left')
            widgets = [line, top, a, b]
            short = brief(entry['hint'])
            if short:
                n = tk.Label(line, text=short, bg=PANEL, fg=MUTED, font=(self.ui, 9), justify='left',
                             wraplength=px(640), cursor='hand2')
                n.pack(anchor='w')
                widgets.append(n)
            for w in widgets:
                w.bind('<Button-1>', lambda _e, e=entry: self.goto(e))

    def goto(self, entry):
        """Open a found setting's page with the setting in view, its name lit for a moment."""
        page = entry['page']
        if page == 'graphics':
            self.var('graphics_view').set('all')
        self.show(page)

        def reveal():
            cell = entry['cell']
            try:
                if not cell.winfo_exists():
                    return
                outer = self.pages[page]
                self.root.update_idletasks()
                y = cell.winfo_rooty() - outer.inner.winfo_rooty()
                total = max(1, outer.inner.winfo_reqheight() + self.px(26))
                outer.canvas.yview_moveto(max(0, y - self.px(70)) / total)
                cell.title.configure(fg=GOLD)
                self.root.after(1600, lambda: cell.title.winfo_exists() and cell.title.configure(fg=TEXT))
            except self.tk.TclError:
                pass
        self.root.after(80, reveal)

    # ---- home: the picture with PLAY, quality, everyday switches, the ready check -------------
    def build_home(self):
        tk, ttk, px = self.tk, self.ttk, self.px
        page = tk.Frame(self.content, bg=W.PAGE)
        self.pages['home'] = page
        wrap = tk.Frame(page, bg=W.PAGE)
        wrap.pack(fill='both', expand=True, padx=(px(12), px(30)), pady=(px(22), 0))
        self.banner = tk.Canvas(wrap, bg=W.PAGE, highlightthickness=0, bd=0, height=px(336))
        self.banner.pack(fill='x')
        self.banner_item = self.banner.create_image(0, 0, anchor='nw')
        self.hero_play = HeroPlay(self, self.banner, self.play)
        self.banner_button = tk.Frame(self.banner, bg='#0b0908')
        ttk.Button(self.banner_button, text=tr('Choose game folder'), style='Accent.TButton',
                   command=lambda: self.choose_game_files('folder')).pack(side='left')
        ttk.Button(self.banner_button, text=tr('Choose disc image (.iso)'),
                   command=lambda: self.choose_game_files('iso')).pack(side='left', padx=(8, 0))
        self.banner.bind('<Configure>', lambda _e: self.root.after_idle(self.draw_banner))

        cols = tk.Frame(wrap, bg=W.PAGE)
        cols.pack(fill='x', pady=(px(14), 0))
        for i, weight in enumerate((13, 10, 10)):
            cols.columnconfigure(i, weight=weight, uniform='home')

        def heading(card, text, link=None, command=None):
            head = tk.Frame(card.body, bg=PANEL)
            head.pack(fill='x', pady=(0, px(6)))
            tk.Label(head, text=text, bg=PANEL, fg=MUTED, font=(self.semi, 10)).pack(side='left')
            if link:
                label = tk.Label(head, text=link + ' ›', bg=PANEL, fg=GOLD, font=(self.semi, 10), cursor='hand2')
                label.pack(side='right')
                label.bind('<Button-1>', lambda _e: command())

        quality = W.Card(self, cols)
        quality.grid(row=0, column=0, sticky='nsew')
        heading(quality, tr('Picture quality'), tr('Fine-tune'), lambda: self.show('graphics'))
        self.quality_name = tk.Label(quality.body, text='', bg=PANEL, fg='#ffffff', font=(self.heading, 16))
        self.quality_name.pack(pady=(px(4), 0))
        self.quality_sub = tk.Label(quality.body, text='', bg=PANEL, fg='#a39d92', font=(self.ui, 10))
        self.quality_sub.pack(pady=(0, px(12)))
        self.quality = W.QualitySlider(self, quality.body, [tr(n) for n in PRESET_NAMES],
                                       lambda i: self.apply_preset(PRESET_ORDER[i]), bg=PANEL)
        self.quality.pack(fill='x')

        everyday = W.Card(self, cols)
        everyday.grid(row=0, column=1, sticky='nsew', padx=(px(14), 0))
        heading(everyday, tr('Everyday'))
        for title, key, on, off in ((tr('Full screen'), 'display_mode', 'fullscreen', 'window'),
                                    (tr('Skip cutscenes'), 'skip_movies', '1', '0'),
                                    (tr('Subtitles'), 'subtitles', '1', '0'),
                                    (tr('FPS counter'), 'perf_overlay', '1', '0')):
            line = tk.Frame(everyday.body, bg=PANEL)
            line.pack(fill='x', pady=px(5))
            tk.Label(line, text=title, bg=PANEL, fg='#e6e0d4', font=(self.ui, 10)).pack(side='left')
            W.Toggle(self, line, self.var(key), on, off, bg=PANEL).pack(side='right')

        ready = W.Card(self, cols)
        ready.grid(row=0, column=2, sticky='nsew', padx=(px(14), 0))
        heading(ready, tr('Ready to play'))
        self.ready = {}
        for key, title in (('disc', tr('Game disc')), ('shaders', tr('Shaders')), ('pad', tr('Controller')),
                           ('save', tr('Save'))):
            line = tk.Frame(ready.body, bg=PANEL)
            line.pack(fill='x', pady=px(5))
            mark = tk.Canvas(line, width=px(22), height=px(22), bg=PANEL, bd=0, highlightthickness=0)
            mark.pack(side='left')
            tk.Label(line, text=title, bg=PANEL, fg='#d7d1c5', font=(self.ui, 10)).pack(side='left', padx=(px(10), 0))
            value = tk.Label(line, text='', bg=PANEL, fg=W.DIM, font=(self.ui, 9))
            value.pack(side='right')
            self.ready[key] = (mark, value)

        news = W.Card(self, wrap, pady=12)
        news.pack(fill='x', pady=(px(14), 0))
        tk.Label(news.body, text=tr('New in {version}', version=VERSION), bg=PANEL, fg=GOLD,
                 font=(self.semi, 10)).pack(side='left')
        link = tk.Label(news.body, text=tr('Read all') + ' ›', bg=PANEL, fg=GOLD, font=(self.semi, 10), cursor='hand2')
        link.pack(side='right')
        link.bind('<Button-1>', lambda _e: webbrowser.open(updater.RELEASES_PAGE))
        rows = [news.body]
        if len(NEWS) > 3:  # the 4th item on: a second row under the first, so the line never runs off the card
            rows.append(tk.Frame(news.body.master, bg=PANEL))
            rows[1].pack(fill='x', padx=px(20), pady=(0, px(12)))
        for i, item in enumerate(NEWS):
            row = rows[min(i // 3, len(rows) - 1)]
            mark = tk.Canvas(row, width=px(8), height=px(8), bg=PANEL, bd=0, highlightthickness=0)
            d = px(4)
            mark.create_polygon(d, 0, 2 * d, d, d, 2 * d, 0, d, fill=W.RED, outline='')
            mark.pack(side='left', padx=(px(22), px(9)))
            tk.Label(row, text=tr(item), bg=PANEL, fg='#d7d1c5', font=(self.ui, 10)).pack(side='left')
        self.refresh_home(checks=True)

    def quality_level(self, s):
        # the level shown: resolution and frame rate (sharpening or filtering alone keep the level)
        return next((i for i, name in enumerate(PRESET_ORDER)
                     if all(str(s.get(k)) == PRESETS[name][k] for k in ('render_scale', 'fps_cap'))), None)

    def refresh_home(self, checks=False):
        """Home's quality card follows the settings; with checks, the ready list is looked at again."""
        if not hasattr(self, 'quality'):
            return
        s = {**self.s, **{k: v.get() for k, v in self.vars.items()}}
        level = self.quality_level(s)
        self.quality.set(level)
        self.quality_name.configure(text=(tr(PRESET_NAMES[level]) if level is not None else tr('Custom')).upper())
        res = dict(SHORT_LABELS['render_scale']).get(str(s.get('render_scale')), '')
        smooth = 'FSR 3.1' if s.get('fsr') == 'aa' else 'SMAA' if s.get('smaa') == '1' else tr('no edge smoothing')
        parts = [res, tr('{fps} FPS', fps=fps_limit(s, self.patches_now(s))), smooth]
        if s.get('fsr') == 'fg':
            parts.append(tr('frame generation'))
        self.quality_sub.configure(text=' · '.join(p for p in parts if p))
        for i, card in enumerate(getattr(self, 'preset_cards', [])):
            card.select(i == level)
        if not checks:
            return
        info, gone = disc_info(s), missing(s, split_ok=True)
        self.mark('disc', bool(info) and not gone,
                  ' '.join(x for x in (tr(info[2]) if info[2] else info[0], info[1]) if x) if info and not gone
                  else tr('Not found'), bad=True)
        shipped = (ROOT / 'scratch' / 'rsx_shader_cache_v2' / 'pso_recipes_ship_v1.bin').is_file()
        self.mark('shaders', shipped, tr('Ready') if shipped else tr('Built while you play'))
        pad = pad_connected()
        self.mark('pad', bool(pad), tr('Connected') if pad else tr('Keyboard') if pad is None else tr('Not found'))
        when = last_save()
        self.mark('save', bool(when), time.strftime('%d %b, %H:%M', time.localtime(when)) if when else tr('None yet'))
        self.last_save_text = time.strftime('%d %b, %H:%M', time.localtime(when)) if when else ''

    def mark(self, key, ok, text, bad=False):
        mark, value = self.ready[key]
        colour = W.GREEN if ok else W.RED_HI if bad else W.AMBER
        mark.delete('all')
        d = self.px(22)
        disc = W.shape(d, d, d // 2, W.mix(PANEL, colour, 0.18), PANEL)
        if disc is not None:
            mark.image = W.photo(disc)
            mark.create_image(0, 0, anchor='nw', image=mark.image)
        if self.icons:
            mark.create_text(d // 2, d // 2 + 1, text=W.ICONS['check' if ok else 'cross' if bad else 'minus'],
                             fill=colour, font=(self.icons, 8))
        else:
            mark.create_text(d // 2, d // 2, text='✓' if ok else '×' if bad else '–', fill=colour,
                             font=(self.ui, 9, 'bold'))
        value.configure(text=text, fg=W.DIM if ok else colour)

    def cover(self, path, w, h):
        """The art scaled to cover w x h with the figure to the right, darkened
        on the left (behind the logo and PLAY), with rounded corners on the page."""
        from PIL import Image
        key = (path, w, h)
        if key in self.bg_cache:
            return self.bg_cache[key]
        with Image.open(path) as im:
            im = im.convert('RGB')
            scale = max(w / im.width, h / im.height) * 1.16
            im = im.resize((max(w, round(im.width * scale)), max(h, round(im.height * scale))), Image.LANCZOS)
            left, top = round((im.width - w) * 0.22), round((im.height - h) * 0.20)
            im = im.crop((left, top, left + w, top + h))
        ramp = Image.new('L', (256, 1))
        ramp.putdata([round(255 * (0.95 - 0.15 * x / 87 if x < 87 else max(0.0, 0.80 * (1 - (x - 87) / 72)))) for x in range(256)])
        im = Image.composite(Image.new('RGB', (w, h), '#080605'), im, ramp.resize((w, h), Image.BILINEAR))
        corners = W.shape(w, h, self.px(16), '#ffffff', '#000000')
        if corners is not None:
            im = Image.composite(im, Image.new('RGB', (w, h), W.PAGE), corners.convert('L'))
        if len(self.bg_cache) > 12:
            self.bg_cache.clear()
        self.bg_cache[key] = im
        return im

    def banner_frame(self, index, w, h):
        from PIL import Image
        im = self.cover(self.bg_sources[index], w, h).copy()
        if self.logo_src:
            if getattr(self, 'logo_banner_size', None) != (w, h):
                with Image.open(self.logo_src) as lg:
                    lg = lg.convert('RGBA')
                    lw = min(int(lg.width * 1.5), self.px(330), int(w * 0.40))   # ICON0's logo is ~300 px wide
                    self.logo_banner = lg.resize((lw, round(lg.height * lw / lg.width)), Image.LANCZOS)
                    self.logo_banner_size = (w, h)
            im.paste(self.logo_banner, (self.px(38), self.px(50)), self.logo_banner)
        return im

    def draw_banner(self, blend=None):
        c, px = self.banner, self.px
        w, h = c.winfo_width(), c.winfo_height()
        if w < 50 or h < 50:
            return
        if not blend:
            c.delete('overlay')
        logo_h = px(76)
        if self.bg_sources:
            from PIL import Image, ImageTk
            if self.bg_size != (w, h):
                self.frames = {}
            self.bg_index %= len(self.bg_sources)
            if self.bg_index not in self.frames:
                self.frames[self.bg_index] = self.banner_frame(self.bg_index, w, h)
            self.bg_size = (w, h)
            im = self.frames[self.bg_index]
            if blend:
                nxt, t = blend
                if nxt not in self.frames:
                    self.frames[nxt] = self.banner_frame(nxt, w, h)
                im = Image.blend(im, self.frames[nxt], t)
            self.banner_photo = ImageTk.PhotoImage(im)
            c.itemconfigure(self.banner_item, image=self.banner_photo)
            if blend:
                return
            if self.logo_src and getattr(self, 'logo_banner', None) is not None:
                logo_h = self.logo_banner.height
        else:
            plain = W.panel(w, h, px(16), '#0b0908', W.PAGE)
            if plain is not None:
                self.banner_photo = W.photo(plain)
                c.itemconfigure(self.banner_item, image=self.banner_photo)
        if not (self.bg_sources and self.logo_src):
            c.create_text(px(40), px(62), text='GOD OF WAR', anchor='w', fill=TEXT, font=(self.heading, 28),
                          tags='overlay')
            c.create_text(px(44), px(62) + px(40), text='A S C E N S I O N', anchor='w', fill=GOLD,
                          font=(self.heading, 13), tags='overlay')
        s = {**self.s, **self.gather()}
        gone = missing(s, split_ok=True)
        info = disc_info(s)
        parts = [tr('PC port {version}', version=VERSION)]
        if info:
            if info[2]:
                parts.append(tr('{region} disc', region=tr(info[2])))
            parts.append(tr('version {version}', version=info[1]))
        else:
            parts.append(GAME_ID)
        c.create_text(px(42), px(50) + logo_h + px(22), text=' · '.join(parts), anchor='w', fill='#cfc8bb',
                      font=(self.ui, 10), tags='overlay')
        pw, ph = self.hero_play.size()
        x, y = px(38), h - px(34) - ph
        self.hero_play.place(x, y)
        c.tag_raise('heroplay')
        saved = getattr(self, 'last_save_text', '')
        c.create_text(x + pw + px(20), y + ph // 2 - px(9), text=tr('Continue') if saved else tr('New game'),
                      anchor='w', fill='#f1ebdf', font=(self.semi, 11), tags='overlay')
        c.create_text(x + pw + px(20), y + ph // 2 + px(10),
                      text=tr('Last save: {when}', when=saved) if saved else tr('No save yet'), anchor='w',
                      fill='#b9b2a5', font=(self.ui, 9), tags='overlay')
        if gone:
            cx, cy = int(w * 0.70), h // 2
            c.create_rectangle(cx - px(250), cy - px(62), cx + px(250), cy + px(48), fill='#0b0908', outline=W.LINE,
                               tags='overlay')
            c.create_text(cx, cy - px(32), text=tr('The game files were not found.'), fill=TEXT,
                          font=(self.ui, 12, 'bold'), tags='overlay')
            c.create_text(cx, cy - px(10), text=tr('Pick your game folder (with PS3_GAME) or disc image.'), fill=MUTED,
                          font=(self.ui, 10), tags='overlay')
            c.create_window(cx, cy + px(22), window=self.banner_button, tags='overlay')

    # ---- graphics -----------------------------------------------------------------------------
    def build_graphics(self):
        tk, px = self.tk, self.px
        f = self.scrolled_page('graphics', tr('Graphics'), tr('How the game appears on your screen.'))
        page = self.pages['graphics']
        view = self.var('graphics_view')
        W.Segmented(self, page.head, view, [('simple', tr('Simple')), ('all', tr('All settings'))],
                    bg=W.PAGE).pack(side='right', padx=(0, px(14)))
        simple = tk.Frame(f, bg=W.PAGE)
        full = tk.Frame(f, bg=W.PAGE)
        for frame in (simple, full):
            frame.columnconfigure(0, weight=1)

        def swap(*_):
            simple.grid_remove()
            full.grid_remove()
            (full if view.get() == 'all' else simple).grid(row=0, column=0, sticky='we')
            page.canvas.yview_moveto(0)
        view.trace_add('write', swap)
        self.indexing = False                 # search finds each setting once, in All settings
        self.build_graphics_simple(simple)
        self.indexing = True
        self.build_graphics_all(full)
        swap()

    def build_graphics_simple(self, f):
        tk, px = self.tk, self.px
        cards = tk.Frame(f, bg=W.PAGE)
        cards.grid(row=0, column=0, columnspan=2, sticky='we')
        res = dict(SHORT_LABELS['render_scale'])
        self.preset_cards = []
        for i, name in enumerate(PRESET_ORDER):
            cards.columnconfigure(i, weight=1, uniform='preset')
            p = PRESETS[name]
            card = W.PresetCard(self, cards, tr(PRESET_NAMES[i]),
                                [f"{res.get(p['render_scale'], '')} · " + tr('{fps} FPS', fps=p['fps_cap']),
                                 tr(PRESET_NOTES[i])], lambda n=name: self.apply_preset(n))
            card.grid(row=0, column=i, sticky='we', padx=(0 if i == 0 else px(12), 0))
            self.preset_cards.append(card)
        self.card(f)
        if self.has('PS3_RENDER_SCALE'):
            self.row(f, tr('Resolution'), self.choice(f, 'render_scale', RENDER_SCALES),
                     tr('Higher is sharper and heavier on the graphics card.'))
        if self.runtime_patches() or fps_unlocked():
            self.row(f, tr('Frame rate'), self.choice(f, 'fps_cap', FPS_CAPS),
                     tr('30 halves the work, for PCs that cannot hold 60.'))
        else:
            self.row(f, tr('Frame rate'), self.ttk.Label(f, text=tr('{fps} FPS', fps=FPS_LIMIT), style='Value.TLabel'),
                     tr('Tick Unlock FPS on the Patches page to allow 60.'))
        if self.has('LD_SMAA'):
            edge, gen = self.edge_and_generation()
            options = [('off', tr('Off')), ('smaa', 'SMAA')] + ([('fsr', 'FSR 3.1')] if self.has('FSR3_NATIVE_AA') else [])
            self.row(f, tr('Edge smoothing'), W.Segmented(self, f, edge, options, bg=PANEL),
                     tr('FSR 3.1 is the smoothest; SMAA is the lightest.') if len(options) > 2 else
                     tr('Smooths jagged edges on the finished picture.'),
                     badge=tr('NEW') if len(options) > 2 else None)
            if self.has('FSR3_FG'):
                self.row(f, tr('Frame generation'), W.Toggle(self, f, gen, bg=PANEL),
                         tr('Shows twice the frames. Needs a recent GPU and a CPU that holds 30+ FPS.'), badge=tr('NEW'))
        if self.has('PS3_VSYNC'):
            self.row(f, tr('VSync'), self.choice(f, 'vsync', VSYNC_MODES),
                     tr('No tearing. Choose G-Sync / FreeSync if your screen has it.'))
        self.row(f, tr('Full screen'), W.Toggle(self, f, self.var('display_mode'), 'fullscreen', 'window', bg=PANEL),
                 tr('{keys} switches while you play.', keys=self.fullscreen_keys()))
        more = tk.Frame(f, bg=W.PAGE)
        more.grid(row=f.grid_size()[1], column=0, columnspan=2, sticky='w', pady=(px(12), 0))
        tk.Label(more, text=tr('More options (sharpening, colour, aspect ratio, window size) are in'), bg=W.PAGE,
                 fg=MUTED, font=(self.ui, 10)).pack(side='left')
        link = tk.Label(more, text=tr('All settings'), bg=W.PAGE, fg=GOLD, font=(self.semi, 10), cursor='hand2')
        link.pack(side='left', padx=(px(6), 0))
        link.bind('<Button-1>', lambda _e: self.var('graphics_view').set('all'))

    def edge_and_generation(self):
        """Two plain switches over the smaa and fsr settings: edge smoothing
        (off / SMAA / FSR 3.1) and frame generation. FSR does one of its two jobs
        at a time, so turning one on turns the other off."""
        if hasattr(self, 'edge_var'):
            return self.edge_var, self.gen_var
        edge, gen = self.edge_var, self.gen_var = self.tk.StringVar(), self.tk.StringVar()
        smaa, fsr = self.var('smaa'), self.var('fsr')

        def now():
            return ('fsr' if fsr.get() == 'aa' else 'smaa' if smaa.get() == '1' else 'off',
                    '1' if fsr.get() == 'fg' else '0')

        busy = []

        def from_settings(*_):
            if busy:
                return
            e, g = now()
            if edge.get() != e:
                edge.set(e)
            if gen.get() != g:
                gen.set(g)

        def to_settings(*_):
            if busy:
                return
            e, g = edge.get(), gen.get()
            was = now()
            if (e, g) == was or not e or not g:
                return
            busy.append(1)
            try:
                change(e, g, was)
            finally:
                busy.clear()
            from_settings()

        def change(e, g, was):
            if e != was[0]:
                if e == 'fsr':
                    fsr.set('aa')
                else:
                    if fsr.get() == 'aa':
                        fsr.set('0')
                    smaa.set('1' if e == 'smaa' else '0')
            else:
                if g == '1':
                    if fsr.get() == 'aa':
                        smaa.set('1')
                    fsr.set('fg')
                else:
                    fsr.set('0')
        from_settings()
        smaa.trace_add('write', from_settings)
        fsr.trace_add('write', from_settings)
        edge.trace_add('write', to_settings)
        gen.trace_add('write', to_settings)
        return edge, gen

    def build_graphics_all(self, f):
        if self.has('PS3_INGAME_SETTINGS'):
            self.note(f, tr('While playing, F1 (or holding SELECT on the controller) opens these picture settings '
                            'in the game. What you change there is saved here too.'), top=0)
        self.section(f, tr('Display'))
        self.row(f, tr('Display mode'), self.choice(f, 'display_mode', DISPLAY_MODES),
                 tr('In the game, {keys} switches between window and full screen.', keys=self.fullscreen_keys()))
        self.row(f, tr('Window size'), self.choice(f, 'window_size', WINDOW_SIZES),
                 tr("The size of the game's window; the picture is stretched to it.") +
                 (tr(' (in pixels; on a Retina display it takes half that in points).') if MAC else ''))
        if len(GAME_WINDOWS) > 1 and self.has('PS3_PARENT_HWND'):
            self.row(f, tr('Game window'), self.choice(f, 'game_window', GAME_WINDOWS),
                     tr("Its own window is how the game is tested. Inside the launcher's window, the launcher "
                        'becomes the game window until the game closes.'))
        self.check(f, 'hide_while_playing', tr('Hide the launcher while playing'),
                   tr('With the game in its own window: the launcher comes back when the game closes.'))
        self.section(f, tr('Frame rate'))
        cap_more = tr('40 and 45 FPS: steadier than an unreachable 60 on a CPU that cannot hold it, for 120/144 Hz '
                      'or variable-refresh screens; on a 60 Hz screen choose 30 or 60.')
        if self.runtime_patches():
            self.row(f, tr('Frame rate limit'), self.choice(f, 'fps_cap', FPS_CAPS),
                     tr('With the Unlock FPS patch ticked (Patches page) the game\'s clock follows real time, so it '
                        'runs at the right speed at any frame rate, up to 60; 30 FPS halves the work each second, '
                        'for PCs that cannot hold 60. Without the patch the game stays at 30.') + ' ' + cap_more)
        elif fps_unlocked():
            self.row(f, tr('Frame rate limit'), self.choice(f, 'fps_cap', FPS_CAPS),
                     tr('The game is built with the Unlock FPS patch (Patches page): its clock follows real time, '
                        'so it runs at the right speed at any frame rate, up to 60. 30 FPS halves the work each '
                        'second, for PCs that cannot hold 60.') + ' ' + cap_more, badge=tr('UNLOCKED'))
        else:
            self.row(f, tr('Frame rate limit'), self.ttk.Label(f, text=tr('{fps} FPS', fps=FPS_LIMIT), style='Value.TLabel'),
                     tr("God of War: Ascension moves its clock 1/30 s every frame, so above 30 FPS the game itself "
                        'runs faster, not smoother. Tick Unlock FPS on the Patches page to untie the clock and '
                        'allow 60.'), badge=tr('FIXED'))
        self.check(f, 'perf_overlay', tr('FPS counter'),
                   tr("On macOS, FPS and frame time in the window's title.") if MAC else
                   tr('FPS, frame time, speed, CPU and GPU in the top right corner.'))
        # The picture options of RPCS3's renderer: offered once the game reads them.
        live = [(sw, title, key, options, hint) for sw, title, key, options, hint in (
            ('PS3_RENDER_SCALE', tr('Internal resolution'), 'render_scale', RENDER_SCALES,
             tr("The game draws at 1280 x 720; above that it is sharper and heavier on the GPU. Above your screen's "
                'own size the picture is scaled down to it, which also smooths jagged edges and shimmer.')),
            ('PS3_ANISO', tr('Anisotropic filtering'), 'aniso', ANISO_LEVELS, tr('Sharper textures seen at an angle.')),
            ('PS3_ASPECT', tr('Aspect ratio'), 'aspect', ASPECTS,
             tr('Stretching distorts the picture in a window that is not 16:9.')),
            ('PS3_SCALE_FILTER', tr('Image scaling'), 'scale_filter', SCALE_FILTERS,
             tr('How the picture is enlarged to the window.')),
            ('PS3_VSYNC', tr('VSync'), 'vsync', VSYNC_MODES, tr('On: no tearing, but a little more latency.') + ' ' +
             tr('G-Sync / FreeSync (VRR): for variable-refresh screens (turned on in the graphics driver). Each frame '
                'is shown as soon as it is ready, so 40-59 FPS look smooth instead of juddering. Best in full screen; '
                'on other screens it tears like Off.')),
            ('PS3_DISPLAY_MATCH', tr('Match monitor refresh'), 'display_match', ON_OFF,
             tr('Full screen only: a 75 or 144 Hz monitor is set to a rate the frame cap divides (60 FPS: 60, 120 '
                'or 240 Hz) while the game is full screen, so every frame is shown equally long. Off: the '
                "monitor's own rate, with a slight judder.")),
            ('LD_SMAA', tr('Edge smoothing (SMAA)'), 'smaa', ON_OFF,
             tr('Smooths jagged edges on the finished picture. Light on the GPU.')),
            ('FSR3_FG', tr('AMD FSR 3.1'), 'fsr', FSR_MODES,
             tr('Anti-aliasing: smoother edges and less shimmer than SMAA, best when the internal resolution matches '
                'your screen. Frame generation: shows twice as many frames, holds the game at half your refresh and '
                "adds some input delay; only for PCs that cannot hold 60 FPS. Both turn the game's motion blur on "
                'and need Unlock FPS and Disable SPU MLAA (Patches page).')),
            ('LD_SHARPEN', tr('Sharpening (RCAS)'), 'sharpen', SHARPEN_LEVELS,
             tr("AMD's contrast-adaptive sharpening, as RPCS3's FSR option applies it.")),
            ('LD_VIBRANCE', tr('Colour vibrance'), 'vibrance', VIBRANCE_LEVELS,
             tr('More vivid colour and a little more contrast. Off is the original look.')),
        ) if self.has(sw)]
        if self.has('PS3_RSX_RENDERER') and len(RENDERERS) > 1:
            live.insert(0, ('PS3_RSX', tr('Renderer'), 'renderer', RENDERERS,
                            tr("RPCS3: RPCS3's renderer. Live draw: ps3recomp's own drawing.")))
        if live:
            self.section(f, tr('Picture'))
            for _sw, title, key, options, hint in live:
                self.row(f, title, self.choice(f, key, options), hint)
        if self.has('LD_AO'):   # 2026-11-15: ambient occlusion, opt-in
            self.section(f, tr('Lighting'))
            self.check(f, 'ambient_occlusion', tr('Ambient occlusion'),
                       tr('Adds soft contact shadows in corners, under ledges and where characters stand. Not part of the PS3 game. Uses some GPU time; turn it off if something looks wrong.'), badge=tr('NEW'))
        if self.has('LD_FULLRES_FX'):   # 2026-11-17: smoother glow (on by default), a switch to turn it off
            if not self.has('LD_AO'):
                self.section(f, tr('Lighting'))
            self.check(f, 'smooth_glow', tr('Smoother glow at high resolutions'),
                       tr('Makes the glow around fire, magic and bright light smooth instead of dotty when the resolution is above 720p. On by default; it did not lower the frame rate in our tests. On a slow graphics card you can turn it off for a few more frames per second.'))
        if self.has('PS3_NO_SHADOWS'):   # 2026-11-22: dynamic shadows off (flatter look), off by default
            if not self.has('LD_AO') and not self.has('LD_FULLRES_FX'):
                self.section(f, tr('Lighting'))
            self.check(f, 'no_shadows', tr('Disable dynamic shadows (flatter look)'),
                       tr("The game stops drawing its real-time shadow maps; the scene looks brighter and flatter. Can also be switched while playing (F1 menu, Display)."), badge=tr('NEW'))
        if self.has('LD_UBER'):   # host uber5: the ubershader, opt-in (game.game_environment sets its switches)
            self.section(f, tr('Shaders'))
            self.check(f, 'uber_shaders', tr('Fewer shader stutters (NVIDIA, experimental)'),
                       tr('When the game meets a shader it has not built yet, it draws that object for a moment with a slower all-purpose shader instead of pausing. In a big fight this cut the waiting for shaders about three times (tested on an RTX 5080). NVIDIA graphics cards only: on other cards the game turns it off by itself. Uses a little more GPU time while it is working. Experimental: turn it off if something looks wrong.'), badge=tr('NEW'))
        if self.has('GP_PULL'):   # 0.2.0: GPU vertex decoding, opt-in (game.game_environment sets GP_PULL=1 W_VDIRECT=3)
            if not self.has('LD_UBER'):
                self.section(f, tr('Shaders'))
            self.check(f, 'gp_pull', tr('GPU vertex decoding (experimental, faster in busy scenes)'),
                       tr("The graphics card decodes the game's 3D model data itself instead of the processor. Faster in busy scenes, mostly on PCs with few CPU cores. Experimental: turn it off if something looks wrong."), badge=tr('NEW'))
        later = [(title, tr('not available yet')) for sw, title in (
            ('PS3_RSX_RENDERER', tr('Renderer (RPCS3 Vulkan / OpenGL)')), ('PS3_RENDER_SCALE', tr('Internal resolution')),
            ('PS3_ANISO', tr('Anisotropic filtering')), ('PS3_ASPECT', tr('Aspect ratio')),
            ('PS3_SCALE_FILTER', tr('Image scaling (FSR)')), ('PS3_VSYNC', tr('VSync'))) if not self.has(sw)]
        if later:
            self.section(f, tr('Coming later'))
            self.unavailable(f, later, tr("This build draws with ps3recomp's live-draw renderer (Direct3D 12). "
                                          "These options come with RPCS3's renderer, which is not "
                                          'part of this build yet; they show up here once the game supports them.'))

    # ---- state ------------------------------------------------------------------------------
    def watch_changes(self):
        """Every 400 ms: a change that has settled is saved by itself; Home and the
        status line follow the settings."""
        values = self.gather()
        if values != self.saved:
            if values == getattr(self, 'settling', None) and not getattr(self, 'capturing', None):
                self.save(auto=True)
            self.settling = values
        else:
            self.settling = None
        if self.var('music').get() != ('1' if self.music_on() else '0'):
            self.set_music(self.var('music').get() == '1')
        self.refresh_music_label()
        paths = tuple(self.vars[k].get() for k in ('game_exe', 'eboot', 'disc_dir', 'disc_iso') if k in self.vars)
        summary = self.summary({**self.s, **values})
        self.ticks = getattr(self, 'ticks', 0) + 1
        changed = paths != getattr(self, 'watched_paths', paths) or summary != getattr(self, 'watched_summary', summary)
        if changed and not (self.process or self.preparing or self.patch_proc):
            self.refresh_status()
            if self.current == 'home':
                self.draw_banner()
        look = tuple(values.get(k) for k in ('render_scale', 'fps_cap', 'smaa', 'fsr', 'sharpen', 'aniso'))
        checks = self.current == 'home' and self.ticks % 8 == 0 and not self.process
        if look != getattr(self, 'watched_look', None) or checks:
            before = getattr(self, 'last_save_text', '')
            self.refresh_home(checks=checks)
            if checks and before != getattr(self, 'last_save_text', ''):
                self.draw_banner()
        self.watched_paths, self.watched_summary, self.watched_look = paths, summary, look
        self.root.after(400, self.watch_changes)

    def save(self, auto=False):
        before = self.saved if isinstance(getattr(self, 'saved', None), dict) else {}
        if not getattr(self, 'settings_kept', False):
            # once per window: the settings as this launcher found them, beside the file
            self.settings_kept = True
            try:
                if CONFIG_FILE.is_file():
                    shutil.copy2(CONFIG_FILE, CONFIG_FILE.with_name(CONFIG_FILE.name + '.bak'))
            except OSError:
                pass
        self.s.update(self.gather())
        save_settings(self.s)
        self.saved = self.gather()
        self.settling = None
        self.saved_label.configure(text='✓  ' + tr('Saved automatically'))
        if not auto:
            self.refresh_status(tr('Settings saved.'))
        mods = ('mods_enabled', 'mods_order', 'mods_dir', 'sound_mix_104', 'update_pkg')
        if not auto or any(before.get(k) != self.saved.get(k) for k in mods):
            threading.Thread(target=prebuild_mods, args=(dict(self.s),), daemon=True).start()
