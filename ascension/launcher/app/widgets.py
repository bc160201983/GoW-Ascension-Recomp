"""The launcher's own controls ("Clean" look): rounded cards, switches, segmented
choices, pill buttons, the quality slider, navigation items and tooltips. Tk has
no rounded shapes, so they are drawn with Pillow (smooth edges) onto canvases;
without Pillow they fall back to plain rectangles."""

try:
    import tkinter as tk
    from tkinter import font as tkfont
except ImportError:          # --play on a system without Tk: the window is never built
    tk = tkfont = None
try:
    from PIL import Image, ImageDraw, ImageTk
except ImportError:
    Image = ImageDraw = ImageTk = None

# Colours. PAGE is the window, CARD a panel on it, RAISED a thing on a panel.
PAGE, CARD, RAISED, LINE, FIELD = '#101012', '#17171a', '#1f1f23', '#26262b', '#0b0b0d'
TEXT, SOFT, MUTED, DIM = '#f1ebdf', '#d7d1c5', '#8f897e', '#6a655c'
GOLD, GOLD_HI, INK = '#d9a441', '#e7b85c', '#15110a'
RED, RED_HI, RED_DK, GREEN, AMBER, TRACK = '#c0202a', '#d0262e', '#96141a', '#62cf7a', '#d9a441', '#3a3a40'

# Glyphs of Windows' icon font (Segoe Fluent Icons on 11, Segoe MDL2 Assets on 10).
ICONS = {'online': '\ue716', 'trophies': '\ue734', 'home': '', 'graphics': '', 'game': '', 'controls': '', 'patches': '',
         'mods': '', 'advanced': '', 'support': '', 'about': '', 'search': '',
         'play': '', 'check': '', 'more': '', 'cross': '', 'minus': ''}

_Canvas = tk.Canvas if tk else object
_Frame = tk.Frame if tk else object
_shapes = {}


def rgb(colour):
    c = colour.lstrip('#')
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    """Colour a moved t (0..1) of the way to b."""
    ra, rb = rgb(a), rgb(b)
    return '#%02x%02x%02x' % tuple(round(x + (y - x) * t) for x, y in zip(ra, rb))


def shape(w, h, r, fill, bg=None, outline=None, width=1, fill2=None):
    """A rounded rectangle w x h (corner radius r) with smooth edges: `fill`
    (to `fill2` downwards, when given), an optional outline, on `bg` or, with
    bg None, on nothing (RGBA). A PIL image, or None without Pillow."""
    if Image is None or w < 2 or h < 2:
        return None
    key = (w, h, r, fill, bg, outline, width, fill2)
    im = _shapes.get(key)
    if im is None:
        k = 4
        W, H = w * k, h * k
        mask = Image.new('L', (W, H), 0)
        ImageDraw.Draw(mask).rounded_rectangle((0, 0, W - 1, H - 1), r * k, fill=255)
        if fill2:
            body = Image.new('RGB', (1, 2))
            body.putdata([rgb(fill), rgb(fill2)])
            body = body.resize((W, H), Image.BILINEAR)
        else:
            body = Image.new('RGB', (W, H), rgb(fill))
        if outline:
            ring = Image.new('RGB', (W, H), rgb(outline))
            inner = Image.new('L', (W, H), 0)
            ImageDraw.Draw(inner).rounded_rectangle((width * k, width * k, W - 1 - width * k, H - 1 - width * k),
                                                    max(0, (r - width) * k), fill=255)
            ring.paste(body, (0, 0), inner)
            body = ring
        if bg is None:
            im = body.convert('RGBA')
            im.putalpha(mask)
        else:
            im = Image.new('RGB', (W, H), rgb(bg))
            im.paste(body, (0, 0), mask)
        im = im.resize((w, h), Image.LANCZOS)
        if len(_shapes) > 600:
            _shapes.clear()
        _shapes[key] = im
    return im


def panel(w, h, r, fill, bg, outline=None, width=1):
    """A big rounded panel: only its corners are drawn smooth, the rest is filled
    (cards are redrawn whenever the window is resized)."""
    if Image is None or w < 2 or h < 2:
        return None
    r = max(1, min(r, w // 2, h // 2))
    if outline:
        if w * h <= 700 * 260:
            return shape(w, h, r, fill, bg, outline, width)
        im = Image.new('RGB', (w, h), rgb(outline))
        im.paste(Image.new('RGB', (w - 2 * width, h - 2 * width), rgb(fill)), (width, width))
        tile = shape(4 * r, 4 * r, r, fill, bg, outline, width)
    else:
        im = Image.new('RGB', (w, h), rgb(fill))
        tile = shape(4 * r, 4 * r, r, fill, bg)
    t = 4 * r
    for (sx, sy), (dx, dy) in (((0, 0), (0, 0)), ((t - r, 0), (w - r, 0)), ((0, t - r), (0, h - r)),
                               ((t - r, t - r), (w - r, h - r))):
        im.paste(tile.crop((sx, sy, sx + r, sy + r)), (dx, dy))
    return im


def photo(image):
    return ImageTk.PhotoImage(image) if image is not None and ImageTk is not None else None


class Card(_Frame):
    """A rounded panel. Put things in .body (a plain frame of the panel's colour)."""

    def __init__(self, app, parent, fill=CARD, back=PAGE, radius=14, padx=20, pady=14, outline=None):
        super().__init__(parent, bg=back, bd=0, highlightthickness=0)
        self.app, self.fill, self.back, self.outline = app, fill, back, outline
        self.r = app.px(radius)
        inset = max(3, round(self.r * 0.3))            # inside this the corners no longer cut in
        self.art = tk.Label(self, bd=0, bg=fill if Image is None else back, highlightthickness=0)
        self.art.place(x=0, y=0, relwidth=1, relheight=1)
        self.body = tk.Frame(self, bg=fill, bd=0, highlightthickness=0)
        self.body.pack(fill='both', expand=True, padx=max(inset, app.px(padx)), pady=max(inset, app.px(pady)))
        self.size = None
        self.bind('<Configure>', self.draw)

    def draw(self, event=None):
        if event is not None and event.widget is not self:
            return
        size = (self.winfo_width(), self.winfo_height())
        if size == self.size or min(size) < 6:
            return
        self.size = size
        image = panel(size[0], size[1], self.r, self.fill, self.back, self.outline, max(1, self.app.px(2)))
        if image is not None:
            self.image = photo(image)
            self.art.configure(image=self.image)

    def set_outline(self, colour):
        if colour != self.outline:
            self.outline, self.size = colour, None
            self.draw()


class Toggle(_Canvas):
    """An on/off switch over a Tk variable (on/off: the values it stores)."""

    def __init__(self, app, parent, var, on='1', off='0', command=None, bg=CARD, accent=GOLD):
        self.app, self.var, self.on, self.off, self.command = app, var, on, off, command
        self.back, self.accent, self.disabled = bg, accent, False
        self.w, self.h = app.px(44), app.px(24)
        super().__init__(parent, width=self.w, height=self.h, bg=bg, bd=0, highlightthickness=0, cursor='hand2')
        self.item = self.create_image(0, 0, anchor='nw')
        self.bind('<Button-1>', self.click)
        self.trace = var.trace_add('write', lambda *_: self.draw())
        self.bind('<Destroy>', self.gone)
        self.draw()

    def gone(self, _event):
        try:
            self.var.trace_remove('write', self.trace)
        except (tk.TclError, ValueError):
            pass

    def is_on(self):
        try:
            return self.var.get() == self.on
        except tk.TclError:
            return False

    def draw(self):
        try:
            if not self.winfo_exists():
                return
        except tk.TclError:
            return
        on, w, h = self.is_on(), self.w, self.h
        track = (mix(self.accent, self.back, 0.55) if self.disabled else self.accent) if on else TRACK
        knob = '#ffffff' if on and not self.disabled else '#c9c3b8'
        base = shape(w, h, h // 2, track, self.back)
        if base is None:
            self.delete('plain')
            self.create_rectangle(0, 0, w, h, fill=track, outline='', tags='plain')
            d = h - 6
            x = w - d - 3 if on else 3
            self.create_rectangle(x, 3, x + d, 3 + d, fill=knob, outline='', tags='plain')
            return
        im = base.copy()
        d = h - 2 * max(2, round(h * 0.125))
        dot = shape(d, d, d // 2, knob, None)
        m = (h - d) // 2
        im.paste(dot, (w - d - m if on else m, m), dot)
        self.image = photo(im)
        self.itemconfigure(self.item, image=self.image)

    def click(self, _event=None):
        if self.disabled:
            return
        self.var.set(self.off if self.is_on() else self.on)
        if self.command:
            self.command()

    def configure(self, cnf=None, **kw):
        state = kw.pop('state', None)
        if state is not None:
            self.disabled = str(state) == 'disabled'
            super().configure(cursor='' if self.disabled else 'hand2')
            self.draw()
        if cnf or kw:
            return super().configure(cnf, **kw)
        return None

    config = configure


class Segmented(_Canvas):
    """A few choices side by side; the chosen one is filled. `options` is
    [(value, label)] over a Tk variable."""

    def __init__(self, app, parent, var, options, bg=CARD, command=None, accent=GOLD, size=10):
        self.app, self.var, self.command, self.back, self.accent = app, var, command, bg, accent
        self.options = [(str(v), str(label)) for v, label in options]
        self.font = (app.ui, size)
        self.bold = (app.ui, size, 'bold')
        measure = tkfont.Font(root=parent, font=self.bold)
        self.inset = max(2, app.px(3))
        pad = app.px(13)
        self.widths = [measure.measure(label) + 2 * pad for _v, label in self.options]
        self.h = app.px(32)
        total = sum(self.widths) + 2 * self.inset
        super().__init__(parent, width=total, height=self.h, bg=bg, bd=0, highlightthickness=0, cursor='hand2')
        self.total = total
        self.bind('<Button-1>', self.click)
        self.trace = var.trace_add('write', lambda *_: self.draw())
        self.bind('<Destroy>', self.gone)
        self.draw()

    def gone(self, _event):
        try:
            self.var.trace_remove('write', self.trace)
        except (tk.TclError, ValueError):
            pass

    def draw(self):
        try:
            if not self.winfo_exists():
                return
            current = str(self.var.get())
        except tk.TclError:
            return
        self.delete('all')
        r = self.app.px(8)
        base = shape(self.total, self.h, r, FIELD, self.back, LINE, 1)
        if base is not None:
            self.base = photo(base)
            self.create_image(0, 0, anchor='nw', image=self.base)
        else:
            self.create_rectangle(0, 0, self.total, self.h, fill=FIELD, outline=LINE)
        x = self.inset
        self.picked = None
        for (value, label), w in zip(self.options, self.widths):
            on = value == current
            if on:
                seg = shape(w, self.h - 2 * self.inset, max(2, r - self.inset), self.accent, FIELD)
                if seg is not None:
                    self.picked = photo(seg)
                    self.create_image(x, self.inset, anchor='nw', image=self.picked)
                else:
                    self.create_rectangle(x, self.inset, x + w, self.h - self.inset, fill=self.accent, outline='')
            self.create_text(x + w // 2, self.h // 2, text=label, fill=INK if on else '#b7b1a6',
                             font=self.bold if on else self.font)
            x += w

    def click(self, event):
        x = self.inset
        for (value, _label), w in zip(self.options, self.widths):
            if x <= event.x < x + w:
                if str(self.var.get()) != value:
                    self.var.set(value)
                    if self.command:
                        self.command()
                return
            x += w


class PillButton(_Canvas):
    """A rounded button drawn on a canvas. kind: 'play' (red, Cinzel), 'gold'
    (outlined, gold text) or 'plain'."""

    def __init__(self, app, parent, text, command, width, height, bg=PAGE, kind='play', icon=None, size=None):
        self.app, self.command, self.kind, self.icon, self.back = app, command, kind, icon, bg
        self.text, self.disabled, self.hot = text, False, False
        self.w, self.h = app.px(width), app.px(height)
        self.size = size or (15 if kind == 'play' else 10)
        super().__init__(parent, width=self.w, height=self.h, bg=bg, bd=0, highlightthickness=0, cursor='hand2')
        self.bind('<Button-1>', lambda _e: not self.disabled and self.command())
        self.bind('<Enter>', lambda _e: self.hover(True))
        self.bind('<Leave>', lambda _e: self.hover(False))
        self.draw()

    def hover(self, on):
        self.hot = on
        self.draw()

    def draw(self):
        self.delete('all')
        w, h, r = self.w, self.h, self.h // 2
        if self.kind == 'play':
            top, low = (RED_HI, RED_DK) if not self.hot else (mix(RED_HI, '#ffffff', 0.10), mix(RED_DK, '#ffffff', 0.08))
            if self.disabled:
                top = low = '#3a2426'
            image, fg = shape(w, h, r, top, self.back, fill2=low), '#ffffff' if not self.disabled else '#a08e8c'
            font = (self.app.heading, self.size)
        elif self.kind == 'gold':
            image = shape(w, h, min(r, self.app.px(10)), mix(CARD, GOLD, 0.10) if self.hot else self.back, self.back,
                          mix(GOLD, self.back, 0.45), 1)
            fg, font = GOLD, (self.app.ui, self.size, 'bold')
        else:
            image = shape(w, h, min(r, self.app.px(9)), '#2c2c32' if self.hot else RAISED, self.back)
            fg, font = TEXT, (self.app.ui, self.size)
        if image is not None:
            self.image = photo(image)
            self.create_image(0, 0, anchor='nw', image=self.image)
        else:
            self.create_rectangle(0, 0, w, h, fill=RED if self.kind == 'play' else RAISED, outline='')
        glyph = ICONS.get(self.icon) if self.icon and self.app.icons else None
        if glyph:
            label = self.create_text(0, h // 2, text=self.text, fill=fg, font=font, anchor='w')
            mark = self.create_text(0, h // 2, text=glyph, fill=fg, anchor='w',
                                    font=(self.app.icons, max(8, round(self.size * 0.72))))
            lw = self.bbox(label)[2] - self.bbox(label)[0]
            mw = self.bbox(mark)[2] - self.bbox(mark)[0]
            gap = self.app.px(10)
            x = (w - (mw + gap + lw)) // 2
            self.coords(mark, x, h // 2 + 1)
            self.coords(label, x + mw + gap, h // 2)
        else:
            self.create_text(w // 2, h // 2, text=self.text, fill=fg, font=font)

    def configure(self, cnf=None, **kw):
        redraw = False
        if 'text' in kw:
            self.text, redraw = kw.pop('text'), True
        if 'state' in kw:
            self.disabled, redraw = str(kw.pop('state')) == 'disabled', True
            super().configure(cursor='' if self.disabled else 'hand2')
        if redraw:
            self.draw()
        if cnf or kw:
            return super().configure(cnf, **kw)
        return None

    config = configure


class Several:
    """The same configure() on several buttons (the big PLAY on Home and the
    small one in the bottom bar)."""

    def __init__(self, *widgets):
        self.widgets = widgets

    def configure(self, **kw):
        for w in self.widgets:
            w.configure(**kw)

    config = configure


class NavItem(_Canvas):
    """A page in the sidebar: an icon and its name, on a rounded bar when it is the page shown."""

    def __init__(self, app, parent, name, title, command, width):
        self.app, self.name, self.title, self.command = app, name, title, command
        self.w, self.h = width, app.px(40)
        self.on = self.hot = False
        super().__init__(parent, width=self.w, height=self.h, bg=PAGE, bd=0, highlightthickness=0, cursor='hand2')
        self.bind('<Button-1>', lambda _e: self.command(self.name))
        self.bind('<Enter>', lambda _e: self.hover(True))
        self.bind('<Leave>', lambda _e: self.hover(False))
        self.draw()

    def hover(self, on):
        self.hot = on
        self.draw()

    def select(self, on):
        if on != self.on:
            self.on = on
            self.draw()

    def draw(self):
        self.delete('all')
        fill = RAISED if self.on else '#18181b' if self.hot else None
        if fill:
            image = shape(self.w, self.h, self.app.px(9), fill, PAGE)
            if image is not None:
                self.image = photo(image)
                self.create_image(0, 0, anchor='nw', image=self.image)
            else:
                self.create_rectangle(0, 0, self.w, self.h, fill=fill, outline='')
        x = self.app.px(14)
        glyph = ICONS.get(self.name) if self.app.icons else None
        if glyph:
            self.create_text(x, self.h // 2 + 1, text=glyph, anchor='w', fill=GOLD if self.on else '#a39d92',
                             font=(self.app.icons, 12))
            x += self.app.px(30)
        self.create_text(x, self.h // 2, text=self.title, anchor='w', fill='#ffffff' if self.on else '#a39d92',
                         font=(self.app.ui, 11, 'bold') if self.on else (self.app.ui, 11))


class QualitySlider(_Canvas):
    """A track with a few named stops; choose(index) is called when one is picked.
    set(index) shows the thumb there, set(None) hides it (a custom mix)."""

    def __init__(self, app, parent, names, choose, bg=CARD):
        self.app, self.names, self.choose, self.back = app, list(names), choose, bg
        self.index = None
        super().__init__(parent, height=app.px(58), bg=bg, bd=0, highlightthickness=0, cursor='hand2')
        self.bind('<Configure>', lambda _e: self.draw())
        self.bind('<Button-1>', self.pick)
        self.bind('<B1-Motion>', self.pick)

    def xs(self):
        w, pad = self.winfo_width(), self.app.px(16)
        n = len(self.names)
        return [pad + (w - 2 * pad) * i / (n - 1) for i in range(n)]

    def set(self, index):
        if index != self.index:
            self.index = index
            self.draw()

    def pick(self, event):
        xs = self.xs()
        i = min(range(len(xs)), key=lambda k: abs(xs[k] - event.x))
        if i != self.index:
            self.index = i
            self.draw()
            self.choose(i)

    def draw(self):
        self.delete('all')
        if self.winfo_width() < 40:
            return
        xs, y, t = self.xs(), self.app.px(16), max(4, self.app.px(6))
        self.create_line(xs[0], y, xs[-1], y, fill='#2a2a2f', width=t, capstyle='round')
        if self.index is not None:
            self.create_line(xs[0], y, xs[self.index], y, fill=GOLD, width=t, capstyle='round')
        d = max(3, self.app.px(4))
        for i, x in enumerate(xs):
            passed = self.index is not None and i <= self.index
            self.create_oval(x - d, y - d, x + d, y + d, fill=GOLD if passed else '#3a3a40', outline=self.back)
        if self.index is not None:
            x, k = xs[self.index], self.app.px(11)
            knob = shape(2 * k, 2 * k, k, '#ffffff', None)
            if knob is not None:
                self.knob = photo(knob)
                self.create_image(x, y, image=self.knob)
            else:
                self.create_oval(x - k, y - k, x + k, y + k, fill='#ffffff', outline='')
        for i, (x, name) in enumerate(zip(xs, self.names)):
            on = i == self.index
            anchor = 'nw' if i == 0 else 'ne' if i == len(xs) - 1 else 'n'
            tx = x - self.app.px(16) if i == 0 else x + self.app.px(16) if i == len(xs) - 1 else x
            self.create_text(tx, y + self.app.px(18), text=name, anchor=anchor, fill='#ffffff' if on else DIM,
                             font=(self.app.ui, 9, 'bold') if on else (self.app.ui, 9))


class PresetCard(_Canvas):
    """A big choice (Graphics, Simple view): a title and two lines; gold outline when chosen."""

    def __init__(self, app, parent, title, lines, command):
        self.app, self.title, self.lines, self.command = app, title, lines, command
        self.on = self.hot = False
        super().__init__(parent, height=app.px(92), bg=PAGE, bd=0, highlightthickness=0, cursor='hand2')
        self.bind('<Configure>', lambda _e: self.draw())
        self.bind('<Button-1>', lambda _e: self.command())
        self.bind('<Enter>', lambda _e: self.hover(True))
        self.bind('<Leave>', lambda _e: self.hover(False))

    def hover(self, on):
        self.hot = on
        self.draw()

    def select(self, on):
        if on != self.on:
            self.on = on
            self.draw()

    def draw(self):
        self.delete('all')
        w, h = self.winfo_width(), self.winfo_height()
        if w < 30:
            return
        fill = '#1c1a15' if self.on else RAISED if self.hot else CARD
        image = panel(w, h, self.app.px(14), fill, PAGE, GOLD if self.on else None, max(1, self.app.px(2)))
        if image is not None:
            self.image = photo(image)
            self.create_image(0, 0, anchor='nw', image=self.image)
        else:
            self.create_rectangle(0, 0, w, h, fill=fill, outline=GOLD if self.on else '')
        x = self.app.px(16)
        self.create_text(x, self.app.px(14), text=self.title, anchor='nw', fill='#ffffff', font=(self.app.ui, 12, 'bold'))
        self.create_text(x, self.app.px(40), text='\n'.join(self.lines), anchor='nw', fill='#a39d92',
                         font=(self.app.ui, 9), width=w - 2 * x)
        if self.on:
            k = self.app.px(11)
            cx, cy = w - self.app.px(14) - k, self.app.px(14) + k
            dot = shape(2 * k, 2 * k, k, GOLD, None)
            if dot is not None:
                self.dot = photo(dot)
                self.create_image(cx, cy, image=self.dot)
            else:
                self.create_oval(cx - k, cy - k, cx + k, cy + k, fill=GOLD, outline='')
            if self.app.icons:
                self.create_text(cx, cy + 1, text=ICONS['check'], fill=INK, font=(self.app.icons, 8))
            else:
                self.create_text(cx, cy, text='✓', fill=INK, font=(self.app.ui, 9, 'bold'))


class Tooltip:
    """The whole explanation of a setting, shown while the mouse rests on it."""

    def __init__(self, app, widget, text):
        self.app, self.widget, self.text = app, widget, text
        self.tip = self.job = None
        widget.bind('<Enter>', self.enter, add='+')
        widget.bind('<Leave>', self.leave, add='+')
        widget.bind('<ButtonPress>', self.leave, add='+')

    def enter(self, _event):
        self.leave()
        self.job = self.widget.after(450, self.show)

    def leave(self, _event=None):
        if self.job:
            try:
                self.widget.after_cancel(self.job)
            except tk.TclError:
                pass
            self.job = None
        if self.tip:
            try:
                self.tip.destroy()
            except tk.TclError:
                pass
            self.tip = None

    def show(self):
        self.job = None
        try:
            if not self.widget.winfo_exists():
                return
            x = self.widget.winfo_rootx()
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + self.app.px(6)
            tip = self.tip = tk.Toplevel(self.widget)
            tip.wm_overrideredirect(True)
            tip.configure(bg=LINE)
            tk.Label(tip, text=self.text, bg='#222226', fg=SOFT, font=(self.app.ui, 9), justify='left',
                     wraplength=self.app.px(420), padx=self.app.px(12), pady=self.app.px(9)).pack(padx=1, pady=1)
            tip.update_idletasks()
            x = max(0, min(x, tip.winfo_screenwidth() - tip.winfo_reqwidth() - 8))
            if y + tip.winfo_reqheight() > tip.winfo_screenheight() - 8:
                y = self.widget.winfo_rooty() - tip.winfo_reqheight() - self.app.px(6)
            tip.wm_geometry(f'+{x}+{y}')
        except tk.TclError:
            self.tip = None
