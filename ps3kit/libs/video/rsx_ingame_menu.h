/* In-game settings menu for the D3D12 live-draw renderer (ovl, 2026-11-11). Included once by rsx_live_draw.c,
 * before ld_pp_present.
 *
 * F1 on the keyboard, or SELECT held alone for half a second on the controller, opens a settings screen over the running
 * game (ovl4: console style, categories on the left): L1 / R1 (Q / E, Page Up / Down) picks the category, up/down an
 * option, left/right or Cross / Enter changes it, Esc / Circle / F1 closes it. What the renderer can change while
 * playing changes at once (tag LIVE); the rest is saved and says AFTER RESTART.
 *
 *   LD_INGAME_MENU=0        no menu at all (nothing is read, nothing is drawn, the pad code does not look)
 *   LD_MENU_KEY=<F1..F12>   the key that opens it (default F1; when the player's key map uses F1, the next free
 *                           F-key up to F10)
 *   LD_MENU_HOLD_MS=<ms>    how long SELECT must be held alone (default 500; cellPad_menu.h)
 *   PS3_INGAME_SETTINGS=<file>  where the changed settings are written at every change, as the launcher's own
 *                           keys and values: {"format": 1, "settings": {"sharpen": "60", ...}}. The launcher passes
 *                           the path and takes the file in. Unset: nothing is written (the panel says so).
 *   LD_MENU_LANG=<en|es|pt-BR>  the panel's language (the launcher passes its own; default English)
 *   LD_MENU_AUTOPAUSE=0     holding SELECT does not hand the game a pause press (cellPad_menu.h)
 *   LD_MENU_SCRIPT=<list>   tests: "t=80:open,t=82:shot,t=84:down,t=85:right,t=90:set:sharpen=0,t=110:close"
 *                           (',' or ';' between events; t = seconds since the first frame; actions open close up
 *                           down left right ok shot set:<key>=<value> sel:<key> lang:<code>). With a script the
 *                           panel ignores the pad and the keys.
 *
 * Never opened: one key check and one flag read per presented frame. While it is open the game's pad is neutral
 * (cellPad_menu.h). The game is paused only through its own pause button: holding SELECT hands the game one short
 * SELECT press when it is not in its pause menu yet (never when it already is); F1 pauses nothing. The panel says
 * which it is.
 *
 * Drawing (ovl4, see "the panel's look" below): one picture, made on a worker thread only when something on it
 * changes (Windows' TrueType text + small anti-aliased shapes, with its own alpha), copied into a texture and blended
 * over the finished frame with one small draw that also fades it in and out -- the credit line's way (ld_credit_*).
 * It sits on the swap chain's buffer, after scaling, SMAA / FSR and sharpening (under frame generation: in the
 * overlay's own layer, ld_fgui_*). */
#ifndef RSX_INGAME_MENU_H
#define RSX_INGAME_MENU_H

#define SRV_MENU (SRV_PP_BASE + LD_PP_N + 3)   /* + 1 = the credit line, + 2 = LD_AO's depth view (rsx_ao.inc) */
typedef char ld_menu_slot_fits[(SRV_MENU < SRV_CPU_HEAP_SLOTS) ? 1 : -1];

/* the pad's side (libs/input/cellPad_menu.h) and the frame pacer (cellGcmSys.c) */
extern volatile long g_pad_menu_enabled, g_pad_menu_block, g_pad_menu_hold_req;
void cellPad_menu_block(int on);
void cellPad_menu_read(unsigned* buttons, int* lx, int* ly);
void cellPad_menu_counters(long long* neutral, long long* with_input, long long* wait_reads, long long* passed,
                           long long* passed_input, long* back_ms);
int cellPad_menu_game_paused(void);
void cellPad_menu_select_counters(long long* withheld, long long* taps, long long* pauses, long long* holds, long* last_pause,
                                  long long* pause_reads);
void cellGcm_set_flip_pace(double fps);
double cellGcm_flip_pace_fps(void);
/* Esc belongs to the panel while it is open (rsx_host_window.h reads it in the window procedure). */
__declspec(selectany) volatile long g_rsx_menu_owns_esc;

/* One game patch switched in the running game (host/ascension_patches.cpp: only patches of one instruction word).
 * on = -1 asks whether it can be switched. A build without it links ld_menu_patch_set_none (-1: cannot). */
int ascension_patch_set(const char* name, int on);
int ld_menu_patch_set_none(const char* name, int on) { (void)name; (void)on; return -1; }
#pragma comment(linker, "/alternatename:ascension_patch_set=ld_menu_patch_set_none")

static int ld_fgui_usable(void);   /* ovl3: the overlay's own layer under frame generation (below) */
enum { MR_FSR, MR_SMAA, MR_SHARP, MR_VIB, MR_VSYNC, MR_CAP, MR_PERF, MR_RS, MR_ANISO, MR_SHADOW, MR_N };
typedef struct { const char* val; const char* text; } ld_menu_choice;
/* The launcher's keys and values (launcher/app/config.py: FSR_MODES, ON_OFF, SHARPEN_LEVELS, VIBRANCE_LEVELS,
 * FPS_CAPS, RENDER_SCALES, ANISO_LEVELS); the texts are short forms of the launcher's. */
static const ld_menu_choice k_menu_fsr[] = { {"0", "Off"}, {"aa", "Anti-aliasing"}, {"fg", "Frame generation"} };
static const ld_menu_choice k_menu_onoff[] = { {"0", "Off"}, {"1", "On"} };
static const ld_menu_choice k_menu_sharp[] = { {"0", "Off"}, {"40", "40%"}, {"60", "60%"}, {"80", "80%"}, {"100", "100%"} };
static const ld_menu_choice k_menu_vib[] = { {"0", "Off"}, {"25", "Low"}, {"50", "Medium"}, {"75", "High"}, {"100", "Very high"} };
/* ovl3 (user, 2026-11-21: "locked 120"): 30 / 60 / 120 (experimental, a real limit on the same pacer as 60) / no limit;
 * a game started with the launcher's 40 or 45 shows that value until another is picked */
static const ld_menu_choice k_menu_cap[] = { {"30", "30 FPS"}, {"60", "60 FPS"}, {"120", "120 FPS (experimental)"},
                                             {"0", "Unlocked (no limit)"} };
static const ld_menu_choice k_menu_rs[] = { {"100", "HD - 1280 x 720"}, {"150", "Full HD - 1920 x 1080"}, {"200", "2K - 2560 x 1440"},
                                            {"300", "4K - 3840 x 2160"}, {"400", "5K - 5120 x 2880"} };
/* host\shadows: the row shows the shadows themselves (On / Off); the value is the launcher's key no_shadows ("1" = off) */
static const ld_menu_choice k_menu_shadow[] = { {"0", "On"}, {"1", "Off"} };
int cellPad_no_shadows(void);            /* libs/input/cellPad.c: PS3_NO_SHADOWS, the game's shadow word */
void cellPad_set_no_shadows(int on);
int cellPad_no_shadows_ok(void);         /* 0: the word is not 0/1 (another eboot): the row is grey */
static const ld_menu_choice k_menu_aniso[] = { {"0", "The game's"}, {"2", "2x"}, {"4", "4x"}, {"8", "8x"}, {"16", "16x"} };

typedef struct {
    const char* label; const char* key; const ld_menu_choice* ch; int n;
    int live;          /* 1: the renderer changes it while playing; 0: at the next start */
    int cur;           /* the chosen entry; -1: the value the game started with is not in the list */
    int changed;       /* changed in this session: written to the settings file */
    char custom[24];   /* what to show for cur = -1 */
} ld_menu_row;

static struct {
    int state;                          /* 0 not started, 1 on, -1 off or failed */
    int open, sel, dirty;
    int vk; char vk_name[8];
    ID3D12PipelineState* pso;
    ID3D12Resource* tex; ID3D12Resource* up; u8* up_map;
    u32 w, h, for_w, for_h;
    int font_added;
    unsigned nav_prev, rep_bit; unsigned long long rep_at;
    int key_was; unsigned esc_guard; unsigned long long sel_since; int sel_armed;
    ld_menu_row row[MR_N];
    int p_mlaa, p_unlock, blur_off_start, blur_switched, can_patch, fsr_dll, obj_start;
    double cap_start;                   /* the frame-rate limit the game was started with (0: none) */
    int fsr_req; char fsr_why[128];
    int paused_shown;
    wchar_t wfile[MAX_PATH]; char file[MAX_PATH * 3];   /* the settings file; the same in UTF-8 for the log */
    int unsaved, save_state;            /* save_state: 0 nothing yet, 1 written, -1 failed */
    int lang;                           /* 0 English, 1 Spanish, 2 Portuguese (LD_MENU_LANG) */
    long long pause_reads0;
    struct { double t; char act[48]; } ev[96]; int n_ev, i_ev; unsigned long long t0;
    unsigned long long open_ms, close_ms, gen0, off0, report_at;
    long long c_neutral0, c_input0, c_passed0, c_passed_in0;
    int report; unsigned opened;
    int busy;                           /* something to do while closed (script, a report, the Esc guard) */
    /* ovl4 */
    int cat;                            /* the category shown (k_menu_cat); sel stays a row id (MR_*) */
    ID3D12PipelineState* pso_plain;     /* the same draw without blending (into the frame-generation layer) */
    u32 tex_bw, tex_bh, req_bw, req_bh; /* the swap-chain size the picture was made / asked for */
    unsigned long long anim_us, close_us; float fade_now, fade_close;
} s_menu = { 0 };

static int ld_menu_enabled(void)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("LD_INGAME_MENU"); on = !(e && e[0] == '0'); }
    return on;
}

/* ---- the panel's language ------------------------------------------------------------------------------------------ */
/* English text -> Spanish, Portuguese (LD_MENU_LANG; the launcher's languages). A text that is not here stays English.
 * Non-ASCII letters are written as UTF-8 bytes so this file stays plain ASCII. The log lines stay English. */
typedef struct { const char* en; const char* es; const char* pt; } ld_menu_tr;
static const ld_menu_tr k_menu_tr[] = {   /* generated by host/ovl/tools/ovl4_tr.py */
    { "SETTINGS",
      "AJUSTES",
      "CONFIGURA\xC3\x87\xC3\x95" "ES" },
    { "Display",
      "Pantalla",
      "Tela" },
    { "Upscaling",
      "Escalado",
      "Upscaling" },
    { "Frame rate",
      "Fotogramas",
      "Taxa de quadros" },
    { "Image",
      "Imagen",
      "Imagem" },
    { "Active now",
      "Activo ahora",
      "Ativo agora" },
    { "%s or hold SELECT: this panel",
      "%s o mant\xC3\xA9n SELECT: este panel",
      "%s ou segure SELECT: este painel" },
    { "Output",
      "Salida",
      "Sa\xC3\xAD" "da" },
    { "Rendering",
      "Renderizado",
      "Renderiza\xC3\xA7\xC3\xA3o" },
    { "Motion blur",
      "Desenfoque de movimiento",
      "Desfoque de movimento" },
    { "Not started",
      "No iniciada",
      "N\xC3\xA3o iniciada" },
    { "Resting",
      "En pausa",
      "Em pausa" },
    { "GAME PAUSED",
      "JUEGO EN PAUSA",
      "JOGO PAUSADO" },
    { "GAME RUNNING",
      "JUEGO EN MARCHA",
      "JOGO RODANDO" },
    { "LIVE",
      "EN VIVO",
      "NA HORA" },
    { "AFTER RESTART",
      "AL REINICIAR",
      "AO REINICIAR" },
    { "SWITCHING",
      "CAMBIANDO",
      "TROCANDO" },
    { "Move",
      "Mover",
      "Mover" },
    { "Adjust",
      "Ajustar",
      "Ajustar" },
    { "Change",
      "Cambiar",
      "Mudar" },
    { "Category",
      "Categor\xC3\xAD" "a",
      "Categoria" },
    { "Close",
      "Cerrar",
      "Fechar" },
    { "Saved at once",
      "Se guarda al instante",
      "Salvo na hora" },
    { "NOT saved: start the game from the launcher",
      "NO se guarda: inicia el juego desde el launcher",
      "N\xC3\x83O \xC3\xA9 salvo: inicie o jogo pelo launcher" },
    { "The settings file could not be written",
      "No se pudo escribir el archivo de ajustes",
      "N\xC3\xA3o foi poss\xC3\xADvel gravar o arquivo de ajustes" },
    { "Edge smoothing (SMAA)",
      "Suavizado (SMAA)",
      "Suaviza\xC3\xA7\xC3\xA3o (SMAA)" },
    { "Sharpening",
      "Nitidez",
      "Nitidez" },
    { "Colour vibrance",
      "Intensidad del color",
      "Vivacidade das cores" },
    { "Frame rate limit",
      "L\xC3\xADmite de FPS",
      "Limite de FPS" },
    { "FPS counter",
      "Contador de FPS",
      "Contador de FPS" },
    { "Internal resolution",
      "Resoluci\xC3\xB3n interna",
      "Resolu\xC3\xA7\xC3\xA3o interna" },
    { "Anisotropic filtering",
      "Filtro anisotr\xC3\xB3pico",
      "Filtro anisotr\xC3\xB3pico" },
    { "Off",
      "Desactivado",
      "Desligado" },
    { "On",
      "Activado",
      "Ligado" },
    { "Anti-aliasing",
      "Antialiasing",
      "Anti-aliasing" },
    { "Frame generation",
      "Gen. de fotogramas",
      "Gera\xC3\xA7\xC3\xA3o de quadros" },
    { "Low",
      "Bajo",
      "Baixo" },
    { "Medium",
      "Medio",
      "M\xC3\xA9" "dio" },
    { "High",
      "Alto",
      "Alto" },
    { "Very high",
      "Muy alto",
      "Muito alto" },
    { "The game's",
      "El del juego",
      "O do jogo" },
    { "No limit",
      "Sin l\xC3\xADmite",
      "Sem limite" },
    { "replaced by FSR",
      "sustituido por FSR",
      "substitu\xC3\xAD" "do pelo FSR" },
    { "120 FPS (experimental)",
      "120 FPS (experimental)",
      "120 FPS (experimental)" },
    { "Unlocked (no limit)",
      "Desbloqueado (sin l\xC3\xADmite)",
      "Destravado (sem limite)" },
    { "Off. Anti-aliasing switches at once; frame generation needs a restart.",
      "Desactivado. El antialiasing cambia al instante; la generaci\xC3\xB3n de fotogramas requiere reiniciar.",
      "Desligado. O anti-aliasing muda na hora; a gera\xC3\xA7\xC3\xA3o de quadros exige reiniciar." },
    { "Off. Frame generation also needs the 'Unlock FPS' patch.",
      "Desactivado. La generaci\xC3\xB3n de fotogramas tambi\xC3\xA9n necesita el parche 'Unlock FPS'.",
      "Desligado. A gera\xC3\xA7\xC3\xA3o de quadros tamb\xC3\xA9m precisa do patch 'Unlock FPS'." },
    { "FSR anti-aliasing replaces SMAA; it can cost speed in heavy fights.",
      "El antialiasing FSR sustituye a SMAA; puede costar velocidad en combates intensos.",
      "O anti-aliasing FSR substitui o SMAA; pode custar velocidade em lutas pesadas." },
    { "Frame generation: the game runs at %.4g FPS, FSR makes every second frame.",
      "Generaci\xC3\xB3n de fotogramas: el juego va a %.4g FPS, FSR crea uno de cada dos.",
      "Gera\xC3\xA7\xC3\xA3o de quadros: o jogo roda a %.4g FPS, o FSR cria um de cada dois." },
    { "Frame generation rests while this panel is open; the game stays at %.4g FPS.",
      "La generaci\xC3\xB3n descansa con este panel abierto; el juego sigue a %.4g FPS.",
      "A gera\xC3\xA7\xC3\xA3o descansa com este painel aberto; o jogo continua a %.4g FPS." },
    { "FSR unavailable: AMD's amd_fidelityfx_*.dll files are not beside the game.",
      "FSR no disponible: faltan los archivos amd_fidelityfx_*.dll junto al juego.",
      "FSR indispon\xC3\xADvel: faltam os arquivos amd_fidelityfx_*.dll junto ao jogo." },
    { "FSR needs the 'Disable SPU MLAA' patch (launcher, Patches page).",
      "FSR necesita el parche 'Disable SPU MLAA' (launcher, p\xC3\xA1gina Parches).",
      "O FSR precisa do patch 'Disable SPU MLAA' (launcher, p\xC3\xA1gina Patches)." },
    { "Frame generation starts with the game: used after a restart.",
      "La generaci\xC3\xB3n de fotogramas arranca con el juego: se usa al reiniciar.",
      "A gera\xC3\xA7\xC3\xA3o de quadros come\xC3\xA7" "a com o jogo: vale ao reiniciar." },
    { "Used after a restart of the game.",
      "Se usa al reiniciar el juego.",
      "Vale ao reiniciar o jogo." },
    { "FSR needs the game's motion blur, off in this session: after a restart.",
      "FSR necesita el desenfoque de movimiento, apagado en esta sesi\xC3\xB3n: al reiniciar.",
      "O FSR precisa do desfoque de movimento, desligado nesta sess\xC3\xA3o: ao reiniciar." },
    { "With frame generation the window must match the internal resolution: after a restart.",
      "Con generaci\xC3\xB3n de fotogramas la ventana debe tener la resoluci\xC3\xB3n interna: al reiniciar.",
      "Com gera\xC3\xA7\xC3\xA3o de quadros a janela precisa ter a resolu\xC3\xA7\xC3\xA3o interna: ao reiniciar." },
    { "AMD's FSR files (amd_fidelityfx_*.dll) could not be loaded.",
      "No se pudieron cargar los archivos FSR de AMD (amd_fidelityfx_*.dll).",
      "N\xC3\xA3o foi poss\xC3\xADvel carregar os arquivos FSR da AMD (amd_fidelityfx_*.dll)." },
    { "The game's motion blur could not be switched on: after a restart.",
      "No se pudo activar el desenfoque de movimiento: al reiniciar.",
      "N\xC3\xA3o foi poss\xC3\xADvel ligar o desfoque de movimento: ao reiniciar." },
    { "FSR could not start in this session (see the log): after a restart.",
      "FSR no pudo iniciarse en esta sesi\xC3\xB3n (ver el registro): al reiniciar.",
      "O FSR n\xC3\xA3o conseguiu iniciar nesta sess\xC3\xA3o (veja o log): ao reiniciar." },
    { "Smooths jagged edges at a small cost.",
      "Suaviza los bordes dentados con poco coste.",
      "Suaviza bordas serrilhadas com pouco custo." },
    { "FSR anti-aliasing does this job while it is on.",
      "El antialiasing FSR hace este trabajo mientras est\xC3\xA1 activado.",
      "O anti-aliasing FSR faz esse trabalho enquanto est\xC3\xA1 ligado." },
    { "Sharpens the final image; too much can look grainy.",
      "Da nitidez a la imagen final; en exceso puede verse granulada.",
      "Deixa a imagem final mais n\xC3\xADtida; demais pode ficar granulada." },
    { "Makes the colours richer.",
      "Hace los colores m\xC3\xA1s intensos.",
      "Deixa as cores mais intensas." },
    { "Syncs frames to the screen: no tearing, slightly more input delay.",
      "Sincroniza con la pantalla: sin cortes en la imagen, algo m\xC3\xA1s de retardo.",
      "Sincroniza com a tela: sem tearing, um pouco mais de atraso." },
    { "Frame generation needs VSync.",
      "La generaci\xC3\xB3n de fotogramas necesita VSync.",
      "A gera\xC3\xA7\xC3\xA3o de quadros precisa do VSync." },
    { "The highest frame rate; the game keeps its real speed at every limit.",
      "El m\xC3\xA1ximo de FPS; el juego mantiene su velocidad real con cualquier l\xC3\xADmite.",
      "O m\xC3\xA1ximo de FPS; o jogo mant\xC3\xA9m a velocidade real em qualquer limite." },
    { "Experimental: needs a fast PC; the FPS counter's OVR line shows missed frames.",
      "Experimental: necesita un PC r\xC3\xA1pido; la l\xC3\xADnea OVR del contador muestra los fotogramas perdidos.",
      "Experimental: precisa de um PC r\xC3\xA1pido; a linha OVR do contador mostra os quadros perdidos." },
    { "No limit: as many frames as this PC and VSync allow.",
      "Sin l\xC3\xADmite: tantos fotogramas como permitan este PC y VSync.",
      "Sem limite: tantos quadros quanto este PC e o VSync permitirem." },
    { "Without the 'Unlock FPS' patch the game is held at 30.",
      "Sin el parche 'Unlock FPS' el juego queda fijo en 30.",
      "Sem o patch 'Unlock FPS' o jogo fica preso em 30." },
    { "Set by frame generation: half of the screen's refresh rate.",
      "Lo fija la generaci\xC3\xB3n de fotogramas: la mitad de la frecuencia de la pantalla.",
      "Definido pela gera\xC3\xA7\xC3\xA3o de quadros: metade da taxa de atualiza\xC3\xA7\xC3\xA3o da tela." },
    { "Shows the frame rate and frame times on screen.",
      "Muestra los FPS y los tiempos de fotograma en pantalla.",
      "Mostra o FPS e os tempos de quadro na tela." },
    { "The resolution the game renders at: higher is sharper but slower.",
      "La resoluci\xC3\xB3n a la que se dibuja el juego: m\xC3\xA1s alta es m\xC3\xA1s n\xC3\xADtida pero m\xC3\xA1s lenta.",
      "A resolu\xC3\xA7\xC3\xA3o em que o jogo \xC3\xA9 desenhado: mais alta \xC3\xA9 mais n\xC3\xADtida, por\xC3\xA9m mais lenta." },
    { "Keeps textures sharp at steep angles; costs little.",
      "Mantiene n\xC3\xADtidas las texturas en \xC3\xA1ngulo; cuesta poco.",
      "Mant\xC3\xA9m as texturas n\xC3\xADtidas em \xC3\xA2ngulos; custa pouco." },
    { "Dynamic shadows",
      "Sombras din\xC3\xA1micas",
      "Sombras din\xC3\xA2micas" },
    { "Real-time shadows. Off: the scene looks brighter and flatter.",
      "Sombras en tiempo real. Apagadas: la escena se ve m\xC3\xA1s clara y plana.",
      "Sombras em tempo real. Desligadas: a cena fica mais clara e plana." },
    { "Not available: this game version has no shadow switch.",
      "No disponible: esta versi\xC3\xB3n del juego no tiene este interruptor.",
      "Indispon\xC3\xADvel: esta vers\xC3\xA3o do jogo n\xC3\xA3o tem esta op\xC3\xA7\xC3\xA3o." },
};
static const char* ML(const char* en)
{
    if (!s_menu.lang || !en || !en[0]) return en;
    for (size_t i = 0; i < sizeof k_menu_tr / sizeof k_menu_tr[0]; i++)
        if (!strcmp(k_menu_tr[i].en, en)) return s_menu.lang == 1 ? k_menu_tr[i].es : k_menu_tr[i].pt;
    return en;
}
static void ld_menu_set_lang(const char* code)
{
    s_menu.lang = !code ? 0 : !_strnicmp(code, "es", 2) ? 1 : !_strnicmp(code, "pt", 2) ? 2 : 0;
    s_menu.dirty = 1;
}

/* ---- what is live right now ------------------------------------------------------------------------------------ */
static int ld_menu_fsr_live(void)   /* 0 off, 1 anti-aliasing, 2 frame generation */
{
    return s_fsr3.scale_on ? 1 : (s_fsr3.on && !s_fsr3.user_off) ? 2 : 0;
}
/* A mode the game could run at all (its patches and AMD's files are there). */
static int ld_menu_fsr_ok(int mode)
{
    if (mode == 0) return 1;
    if (!s_menu.fsr_dll || !s_menu.p_mlaa) return 0;
    return mode == 1 ? 1 : s_menu.p_unlock;
}
/* Anti-aliasing can be switched on while playing: 1, else 0 and why. */
static int ld_menu_aa_live_ok(const char** why)
{
    if (ascension_patch_on("Disable Motion Blur") > 0 && !s_menu.can_patch) {
        *why = "FSR needs the game's motion blur, off in this session: after a restart."; return 0; }
    if (s_fsr3.on && (g.width * g_ld_rs != s_fsr3.w || g.height * g_ld_rs != s_fsr3.h)) {
        *why = "With frame generation the window must match the internal resolution: after a restart."; return 0; }
    *why = ""; return 1;
}
static double ld_menu_user_cap(void)
{
    const ld_menu_row* r = &s_menu.row[MR_CAP];
    if (!s_menu.p_unlock) return 30.0;
    if (r->cur >= 0) return atof(r->ch[r->cur].val);
    return s_menu.cap_start;   /* a limit that is not in the list (0: none), as the game was started */
}

/* ---- FSR switched while playing (at the frame's end, GPU idle) --------------------------------------------------- */
static int ld_menu_aa_on(void)
{
    const char* why = "";
    if (!ld_menu_aa_live_ok(&why)) { snprintf(s_menu.fsr_why, sizeof s_menu.fsr_why, "%s", why); return 0; }
    ld_flush(LD_FLUSH_READBACK);   /* everything recorded and queued is done */
    if (!g.ready) return 0;
    if (!fsr3_dll_load()) {
        snprintf(s_menu.fsr_why, sizeof s_menu.fsr_why, "AMD's FSR files (amd_fidelityfx_*.dll) could not be loaded.");
        return 0;
    }
    if (ascension_patch_on("Disable Motion Blur") > 0) {   /* the objects' motion comes from the blur's draws */
        if (ascension_patch_set("Disable Motion Blur", 0) != 1) {
            SetDllDirectoryW(NULL);
            snprintf(s_menu.fsr_why, sizeof s_menu.fsr_why, "The game's motion blur could not be switched on: after a restart.");
            return 0;
        }
        s_menu.blur_switched = 1;
        fprintf(stderr, "[menu] motion blur switched on in the running game (FSR reads the objects' motion from it)\n");
    }
    const unsigned rw = g.width * g_ld_rs, rh = g.height * g_ld_rs;
    s_fsr3.cam_on = 1; s_fsr3.obj_on = 1; s_fsr3.scale_want = 1; s_fsr3.native = 1; s_fsr3.live_init = 1;
    fsr3_scale_init(g.dev, g.queue, rw, rh, s_fsr3.dev ? s_fsr3.w : rw, s_fsr3.dev ? s_fsr3.h : rh);
    SetDllDirectoryW(NULL);
    if (!s_fsr3.scale_on) {
        s_fsr3.scale_want = 0; s_fsr3.native = 0;
        if (!s_fsr3.on) s_fsr3.obj_on = 0;
        if (s_menu.blur_switched) { ascension_patch_set("Disable Motion Blur", 1); s_menu.blur_switched = 0; }
        snprintf(s_menu.fsr_why, sizeof s_menu.fsr_why, "FSR could not start in this session (see the log): after a restart.");
        return 0;
    }
    s_fsr3.have = 0; s_fsr3.have_prev = 0; s_fsr3.up_prev_ok = 0; s_fsr3.obj_seen = 0; s_fsr3.novel_scene = 0;
    s_fsr3.jit_advance = 0;
    s_hole.on = -1;        /* hole fill: FSR's jitter opens holes at 1x too */
    s_reuse.valid = 0;     /* reused draws carry the old (unjittered) transform */
    s_menu.fsr_why[0] = 0;
    fprintf(stderr, "[menu] FSR anti-aliasing switched ON at frame %u (%ux%u)\n", g_ld_frames, rw, rh);
    return 1;
}
static void ld_menu_aa_off(void)
{
    if (!s_fsr3.upout && !s_fsr3.scale_on) return;
    ld_flush(LD_FLUSH_READBACK);
    if (!g.ready) return;
    fsr3_scale_shutdown();
    if (!s_fsr3.on) s_fsr3.obj_on = 0;   /* no velocity twins without FSR (frame generation keeps them) */
    if (s_menu.blur_switched && !(s_fsr3.on && !s_fsr3.user_off)) {
        ascension_patch_set("Disable Motion Blur", 1); s_menu.blur_switched = 0;
        fprintf(stderr, "[menu] motion blur back off (as the game was started)\n");
    }
    s_hole.on = -1; s_reuse.valid = 0;
    fprintf(stderr, "[menu] FSR anti-aliasing switched OFF at frame %u\n", g_ld_frames);
}
/* Frame generation of a game that was started with it: AMD's swap chain stays; off = real frames pass through. */
static void ld_menu_fg(int on)
{
    if (!s_fsr3.on) return;
    if (on) {
        s_fsr3.user_off = 0; s_fsr3.was_enabled = 0;
        s_fsr3.fl_us = 0; s_fsr3.fl_n = 0; s_fsr3.fl_low = 0; s_fsr3.below = 0;
        const double uc = ld_menu_user_cap();
        const double cap = s_fsr3.fg_cap > 0 && (uc <= 0 || uc > s_fsr3.fg_cap + 0.01) ? s_fsr3.fg_cap : uc;
        cellGcm_set_flip_pace(cap); s_ld_perf_native = cap;
        g_ld_vsync = 1;
        fprintf(stderr, "[menu] frame generation switched ON at frame %u: game paced at %.2f frames/s, VSync on\n", g_ld_frames, cap);
    } else {
        s_fsr3.user_off = 1;
        const double uc = ld_menu_user_cap();
        cellGcm_set_flip_pace(uc); if (uc > 0) s_ld_perf_native = uc;   /* no limit: the counter keeps its target (review note c) */
        g_ld_vsync = s_menu.row[MR_VSYNC].cur != 0;
        fprintf(stderr, "[menu] frame generation switched OFF at frame %u: real frames only, game paced at %.2f frames/s, VSync %s\n",
                g_ld_frames, uc, g_ld_vsync ? "on" : "off");
    }
}
static void ld_menu_fsr_apply(int mode)
{
    const int live = ld_menu_fsr_live();
    if (mode == live) return;
    if (mode != 1) ld_menu_aa_off();
    if (mode != 2 && s_fsr3.on && !s_fsr3.user_off) ld_menu_fg(0);
    if (mode == 1 && !s_fsr3.scale_on) {
        if (s_fsr3.upout) ld_menu_aa_off();   /* FSR's own guard switched it off earlier: start clean */
        ld_menu_aa_on();
    }
    if (mode == 2 && s_fsr3.on && s_fsr3.user_off) ld_menu_fg(1);
    if (mode == 2 && !s_fsr3.on)
        snprintf(s_menu.fsr_why, sizeof s_menu.fsr_why, "Frame generation starts with the game: used after a restart.");
}
/* After the frame's Present, nothing recorded yet for the next one. */
static void ld_menu_after_present(void)
{
    if (s_menu.state <= 0 || s_menu.fsr_req < 0) return;
    const int mode = s_menu.fsr_req;
    s_menu.fsr_req = -1;
    const unsigned long long t0 = fsr3_now_us();
    ld_menu_fsr_apply(mode);
    fprintf(stderr, "[menu] FSR mode asked: %s; live now: %s (%.1f ms)\n", k_menu_fsr[mode].text,
            k_menu_fsr[ld_menu_fsr_live()].text, (double)(fsr3_now_us() - t0) / 1000.0);
    s_menu.dirty = 1;
}

/* ---- rows ---------------------------------------------------------------------------------------------------------- */
static int ld_menu_find(const ld_menu_row* r, const char* val)
{
    for (int i = 0; i < r->n; i++) if (!strcmp(r->ch[i].val, val)) return i;
    return -1;
}
static void ld_menu_row_set(int id, const char* label, const char* key, const ld_menu_choice* ch, int n, int live,
                            const char* start, const char* custom_fmt)
{
    ld_menu_row* r = &s_menu.row[id];
    r->label = label; r->key = key; r->ch = ch; r->n = n; r->live = live; r->changed = 0;
    r->cur = ld_menu_find(r, start);
    if (r->cur < 0) snprintf(r->custom, sizeof r->custom, custom_fmt, start);
}
/* What row id shows: its value text, whether it is grey (cannot be changed now), its tag (LIVE / AFTER RESTART /
 * SWITCHING / none) and the one-line description for the bottom of the panel (warn: a reason, drawn amber). */
static void ld_menu_row_info(int id, char* val, size_t nval, int* grey, const char** tag, char* why, size_t nwhy, int* warn)
{
    const ld_menu_row* r = &s_menu.row[id];
    const int live = ld_menu_fsr_live();
    int w = 0;
    snprintf(val, nval, "%s", r->cur >= 0 ? ML(r->ch[r->cur].text) : !strcmp(r->custom, "No limit") ? ML(r->custom) : r->custom);
    *grey = 0; *tag = r->live ? "LIVE" : "AFTER RESTART";
    const char* d = "";
    switch (id) {
    case MR_FSR:
        if (!s_menu.fsr_dll) { *grey = 1; w = 1; d = "FSR unavailable: AMD's amd_fidelityfx_*.dll files are not beside the game."; break; }
        if (!s_menu.p_mlaa) { *grey = 1; w = 1; d = "FSR needs the 'Disable SPU MLAA' patch (launcher, Patches page)."; break; }
        if (s_menu.fsr_req >= 0) { *tag = "SWITCHING"; d = ""; break; }   /* for one frame: done at this frame's end */
        if (r->cur != live) {
            *tag = "AFTER RESTART"; w = 1;
            const char* lw = "";
            if (r->cur == 1) (void)ld_menu_aa_live_ok(&lw);
            d = s_menu.fsr_why[0] ? s_menu.fsr_why : r->cur == 2 ? "Frame generation starts with the game: used after a restart."
              : lw[0] ? lw : "Used after a restart of the game.";
        } else if (live == 1) d = "FSR anti-aliasing replaces SMAA; it can cost speed in heavy fights.";
        /* without the overlay's layer generation rests under the panel (menu_hold), the game keeps its pace (ld_menu_open) */
        else if (live == 2) { snprintf(why, nwhy, ML(s_menu.open && !ld_fgui_usable()
                                  ? "Frame generation rests while this panel is open; the game stays at %.4g FPS."
                                  : "Frame generation: the game runs at %.4g FPS, FSR makes every second frame."), cellGcm_flip_pace_fps());
                              *warn = 0; return; }
        else if (!s_menu.p_unlock) d = "Off. Frame generation also needs the 'Unlock FPS' patch.";
        else d = "Off. Anti-aliasing switches at once; frame generation needs a restart.";
        break;
    case MR_SMAA:
        d = "Smooths jagged edges at a small cost.";
        if (live == 1) { *grey = 1; *tag = ""; snprintf(val, nval, "%s", ML("replaced by FSR")); d = "FSR anti-aliasing does this job while it is on."; }
        break;
    case MR_SHARP: d = "Sharpens the final image; too much can look grainy."; break;
    case MR_VIB: d = "Makes the colours richer."; break;
    case MR_VSYNC:
        d = "Syncs frames to the screen: no tearing, slightly more input delay.";
        if (live == 2) { *grey = 1; *tag = ""; snprintf(val, nval, "%s", ML("On")); d = "Frame generation needs VSync."; }
        break;
    case MR_CAP:
        d = "The highest frame rate; the game keeps its real speed at every limit.";
        if (!s_menu.p_unlock) { *grey = 1; *tag = ""; w = 1; snprintf(val, nval, "30 FPS"); d = "Without the 'Unlock FPS' patch the game is held at 30."; }
        else if (live == 2) { *grey = 1; *tag = ""; snprintf(val, nval, "%.4g FPS", cellGcm_flip_pace_fps());
                              d = "Set by frame generation: half of the screen's refresh rate."; }
        else if (r->cur >= 0 && !strcmp(r->ch[r->cur].val, "120"))
            d = "Experimental: needs a fast PC; the FPS counter's OVR line shows missed frames.";
        else if (r->cur >= 0 && !strcmp(r->ch[r->cur].val, "0"))
            d = "No limit: as many frames as this PC and VSync allow.";
        break;
    case MR_PERF: d = "Shows the frame rate and frame times on screen."; break;
    case MR_RS: d = "The resolution the game renders at: higher is sharper but slower."; break;
    case MR_ANISO: d = "Keeps textures sharp at steep angles; costs little."; break;
    case MR_SHADOW: if (!cellPad_no_shadows_ok()) { *grey = 1; *tag = ""; w = 1; d = "Not available: this game version has no shadow switch."; break; }
        d = "Real-time shadows. Off: the scene looks brighter and flatter."; break;
    default: break;
    }
    snprintf(why, nwhy, "%s", ML(d));
    *warn = w;
}
/* The row's value goes live (where the renderer can). */
static void ld_menu_apply(int id)
{
    const ld_menu_row* r = &s_menu.row[id];
    if (r->cur < 0) return;
    const char* v = r->ch[r->cur].val;
    switch (id) {
    case MR_FSR:   s_menu.fsr_req = r->cur; s_menu.fsr_why[0] = 0; break;          /* at this frame's end */
    case MR_SMAA:  s_pp.smaa = atoi(v) != 0; break;
    case MR_SHARP: s_pp.sharpen = atoi(v); break;
    case MR_VIB:   s_ld_vibrance_v = atoi(v); break;
    case MR_VSYNC: g_ld_vsync = atoi(v) != 0; break;
    case MR_CAP:   cellGcm_set_flip_pace(atof(v));                                   /* 0: no limit */
                   /* the counter's target: the limit; no limit = 60 (never 0: SPD; no OVR line for a limit that is not there) */
                   s_ld_perf_native = atof(v) > 0 ? atof(v) : 60.0; break;
    case MR_PERF:  if (atoi(v)) { s_ld_perf_on = 1; s_ld_perf_draw = 1; } else s_ld_perf_draw = 0; break;
    case MR_ANISO: (void)ld_aniso_override();                                         /* read once before it is set */
                   s_ld_aniso_v = atoi(v) < 2 ? 0 : atoi(v);
                   s_reuse.valid = 0;       /* the level is part of the sampler key: the next draws ask for new states */
                   break;
    case MR_SHADOW: cellPad_set_no_shadows(atoi(v) != 0); break;                     /* the pad poll writes the game's word */
    default: break;                                                                   /* at the next start */
    }
}
static void ld_menu_save(void);
/* dir: -1 / +1 = the entry before / after; to >= 0: that entry. 1 when the row changed. */
static int ld_menu_change(int id, int dir, int to)
{
    ld_menu_row* r = &s_menu.row[id];
    char val[96], why[320]; int grey, warn = 0; const char* tag;
    ld_menu_row_info(id, val, sizeof val, &grey, &tag, why, sizeof why, &warn);
    if (grey) { fprintf(stderr, "[menu] %s not changed: %s\n", r->key, why); return 0; }
    int n = r->cur;
    if (to >= 0) n = to;
    else if (n < 0) n = dir > 0 ? r->n - 1 : 0;
    else {
        for (int k = 0; k < r->n; k++) {
            n = (n + dir + r->n) % r->n;
            if (id != MR_FSR || ld_menu_fsr_ok(n)) break;
        }
    }
    if (id == MR_FSR && !ld_menu_fsr_ok(n)) {
        fprintf(stderr, "[menu] fsr=%s not possible in this game (patches)\n", k_menu_fsr[n].val); return 0; }
    if (n == r->cur) return 0;
    const char* old = r->cur >= 0 ? r->ch[r->cur].val : r->custom;
    fprintf(stderr, "[menu] %s: %s -> %s (%s)\n", r->key, old, r->ch[n].val, r->live ? "at once" : "at next start");
    r->cur = n; r->changed = 1;
    s_menu.unsaved = 1; s_menu.dirty = 1;
    ld_menu_apply(id);
    ld_menu_save();   /* at every change (the file is tiny): nothing is lost when the game is closed with the panel open */
    return 1;
}

/* ---- saving -------------------------------------------------------------------------------------------------------- */
static void ld_menu_save(void)
{
    if (!s_menu.unsaved) return;
    s_menu.unsaved = 0;
    if (!s_menu.wfile[0]) { fprintf(stderr, "[menu] changes not saved: PS3_INGAME_SETTINGS is not set (start the game from the launcher)\n"); return; }
    /* wide-character paths: a folder name outside the PC's own alphabet works too */
    wchar_t tmp[MAX_PATH + 8]; _snwprintf(tmp, MAX_PATH + 7, L"%ls.tmp", s_menu.wfile); tmp[MAX_PATH + 7] = 0;
    const int was = s_menu.save_state;
    FILE* f = _wfopen(tmp, L"wb");
    if (!f) { s_menu.save_state = -1; if (was != -1) s_menu.dirty = 1; fprintf(stderr, "[menu] could not write %s.tmp\n", s_menu.file); return; }
    fprintf(f, "{\"format\": 1, \"settings\": {");
    int n = 0;
    for (int i = 0; i < MR_N; i++) {
        const ld_menu_row* r = &s_menu.row[i];
        if (!r->changed || r->cur < 0) continue;
        fprintf(f, "%s\"%s\": \"%s\"", n++ ? ", " : "", r->key, r->ch[r->cur].val);
    }
    fprintf(f, "}}\n");
    const int ok = !ferror(f);
    fclose(f);
    if (ok && MoveFileExW(tmp, s_menu.wfile, MOVEFILE_REPLACE_EXISTING)) {
        s_menu.save_state = 1;
        fprintf(stderr, "[menu] %d setting(s) saved for the launcher: %s\n", n, s_menu.file);
    } else {
        s_menu.save_state = -1;
        fprintf(stderr, "[menu] could not save %s (error %lu)\n", s_menu.file, (unsigned long)GetLastError());
        DeleteFileW(tmp);
    }
    if (was != s_menu.save_state) s_menu.dirty = 1;
}

/* ---- ovl4: the panel's look ---------------------------------------------------------------------------------------- */
/* A console-style settings screen: the categories on the left (L1 / R1, Q / E), the chosen category's options on the
 * right (up / down picks one, left / right / Cross changes it: value pickers, sliders, switches), a one-line description
 * of the highlighted option, and a hint bar with the controller's and the keyboard's keys.
 * Text: Windows' own TrueType text drawing (GDI, anti-aliased) with Segoe UI, and the launcher's Cinzel (SIL OFL,
 * shipped beside the launcher as Cinzel-Black.ttf + OFL-Cinzel.txt, passed in PS3_OVERLAY_FONT) for the headings; the
 * category icons come from Windows' Segoe MDL2 Assets (left out when that font is missing). No font or library is added
 * to the release. Shapes (rounded panels, sliders, switches, button glyphs) are drawn by the small anti-aliased routines
 * below into the same picture with their own alpha: a translucent panel, transparent corners, a soft shadow.
 * Everything is sized by the swap chain's height / 720, so it is equally sharp at 1080p, 1440p and 4K.
 * The picture is made on a worker thread from a snapshot of what it shows (ld_menu_view): opening the panel or changing a
 * value never stalls a frame; a frame only copies a finished picture into a texture. */
enum { MKIND_PICK, MKIND_SLIDER, MKIND_TOGGLE };
#define LD_MENU_NCAT 4
static const struct { const char* name; unsigned short icon; int rows[3]; int n; } k_menu_cat[LD_MENU_NCAT] = {
    { "Display",    0xE7F4, { MR_RS, MR_VSYNC, MR_SHADOW }, 3 },          /* Segoe MDL2 Assets: TVMonitor */
    { "Upscaling",  0xE740, { MR_FSR, MR_SMAA, 0 }, 2 },          /* FullScreen */
    { "Frame rate", 0xEC4A, { MR_CAP, MR_PERF, 0 }, 2 },          /* SpeedHigh */
    { "Image",      0xE790, { MR_SHARP, MR_VIB, MR_ANISO }, 3 },  /* Color */
};
static int ld_menu_cat_of(int id)
{
    for (int c = 0; c < LD_MENU_NCAT; c++)
        for (int i = 0; i < k_menu_cat[c].n; i++) if (k_menu_cat[c].rows[i] == id) return c;
    return 0;
}
static int ld_menu_kind(int id)
{
    return id == MR_SMAA || id == MR_VSYNC || id == MR_PERF || id == MR_SHADOW ? MKIND_TOGGLE : id == MR_SHARP || id == MR_VIB ? MKIND_SLIDER : MKIND_PICK;
}

typedef struct {
    char label[96], val[96], tag[48];
    int kind, grey, on, tagk;           /* tagk: 0 none, 1 live, 2 after restart, 3 switching */
    float frac, tick[8]; int nticks;    /* slider: where the value sits (0..1) and its steps */
} ld_menu_vrow;
typedef struct {
    unsigned seq; u32 bw, bh;
    char title[48], pill[48]; int paused;
    char save[128]; int save_bad;
    char cat[LD_MENU_NCAT][48]; int cat_sel;
    ld_menu_vrow row[3]; int nrow, row_sel;
    char desc[320]; int desc_warn;
    char card_title[48], card_k[3][64], card_v[3][64]; int ncard;   /* "Active now": what runs at this moment */
    char open_hint[96];                 /* how to open the panel (under the categories) */
    int dump;                           /* tests (LD_MENU_DUMP + script "dump"): > 0 = also write the picture at other sizes */
    char h_move[40], h_adj[40], h_chg[40], h_cat[40], h_close[40];
} ld_menu_view;

static int s_menu_dump_req, s_menu_dump_n;   /* tests: LD_MENU_SCRIPT "dump" */
/* What the panel shows now (the frame's thread; the worker only draws it). */
static void ld_menu_view_make(ld_menu_view* v, u32 bw, u32 bh)
{
    memset(v, 0, sizeof *v);
    if (s_menu_dump_req) { s_menu_dump_req = 0; v->dump = ++s_menu_dump_n; }
    v->bw = bw; v->bh = bh;
    const int paused = cellPad_menu_game_paused();
    s_menu.paused_shown = paused;
    v->paused = paused == 1;
    snprintf(v->title, sizeof v->title, "%s", ML("SETTINGS"));
    snprintf(v->pill, sizeof v->pill, "%s", ML(paused == 1 ? "GAME PAUSED" : "GAME RUNNING"));
    v->save_bad = !s_menu.wfile[0] || s_menu.save_state < 0;
    snprintf(v->save, sizeof v->save, "%s", ML(!s_menu.wfile[0] ? "NOT saved: start the game from the launcher"
                                               : s_menu.save_state < 0 ? "The settings file could not be written" : "Saved at once"));
    for (int c = 0; c < LD_MENU_NCAT; c++) snprintf(v->cat[c], sizeof v->cat[c], "%s", ML(k_menu_cat[c].name));
    v->cat_sel = s_menu.cat;
    const int live = ld_menu_fsr_live();
    for (int i = 0; i < k_menu_cat[s_menu.cat].n; i++) {
        const int id = k_menu_cat[s_menu.cat].rows[i];
        const ld_menu_row* r = &s_menu.row[id];
        ld_menu_vrow* o = &v->row[v->nrow++];
        char why[320]; int warn = 0; const char* tag = "";
        ld_menu_row_info(id, o->val, sizeof o->val, &o->grey, &tag, why, sizeof why, &warn);
        snprintf(o->label, sizeof o->label, "%s", ML(r->label));
        snprintf(o->tag, sizeof o->tag, "%s", tag[0] ? ML(tag) : "");
        o->tagk = !tag[0] ? 0 : !strcmp(tag, "LIVE") ? 1 : !strcmp(tag, "AFTER RESTART") ? 2 : 3;
        o->kind = ld_menu_kind(id);
        if (o->kind == MKIND_TOGGLE)
            o->on = id == MR_VSYNC && live == 2 ? 1 : id == MR_SMAA && live == 1 ? 0 : id == MR_SHADOW ? r->cur == 0   /* on = shadows drawn (no_shadows "0") */
                  : r->cur >= 0 && atoi(r->ch[r->cur].val) != 0;
        if (o->kind == MKIND_SLIDER) {
            const int x = r->cur >= 0 ? atoi(r->ch[r->cur].val) : atoi(r->custom);
            o->frac = x <= 0 ? 0.0f : x >= 100 ? 1.0f : (float)x / 100.0f;
            for (int t = 0; t < r->n && t < 8; t++) o->tick[o->nticks++] = (float)atoi(r->ch[t].val) / 100.0f;
        }
        if (id == s_menu.sel) { v->row_sel = i; snprintf(v->desc, sizeof v->desc, "%s", why); v->desc_warn = warn; }
    }
    /* the "Active now" card under a category with two options: what the game runs with at this moment */
    { const int fl = ld_menu_fsr_live();
      snprintf(v->card_title, sizeof v->card_title, "%s", ML("Active now"));
#define LD_CARD(k, ...) do { snprintf(v->card_k[v->ncard], sizeof v->card_k[0], "%s", k); \
                             snprintf(v->card_v[v->ncard], sizeof v->card_v[0], __VA_ARGS__); v->ncard++; } while (0)
      const char* fg = !s_fsr3.on ? ML("Not started") : s_fsr3.user_off ? ML("Off") : s_menu.open && !ld_fgui_usable() ? ML("Resting") : ML("On");
      if (k_menu_cat[s_menu.cat].n < 3) switch (s_menu.cat) {
      case 0: LD_CARD(ML("Output"), "%u x %u", bw, bh);
              LD_CARD(ML("Rendering"), "%u x %u", g.width * g_ld_rs, g.height * g_ld_rs);
              break;
      case 1: LD_CARD("AMD FSR 3.1", "%s", ML(k_menu_fsr[fl].text));
              LD_CARD(ML("Motion blur"), "%s", ML(ascension_patch_on("Disable Motion Blur") > 0 ? "Off" : "On"));
              LD_CARD(ML("Frame generation"), "%s", fg); break;
      case 2: { const double pace = cellGcm_flip_pace_fps();
                if (pace > 0) LD_CARD(ML("Frame rate limit"), "%.4g FPS", pace); else LD_CARD(ML("Frame rate limit"), "%s", ML("No limit")); }
              LD_CARD("VSync", "%s", ML(g_ld_vsync ? "On" : "Off"));
              LD_CARD(ML("Frame generation"), "%s", fg); break;
      default: break;
      }
#undef LD_CARD
    }
    snprintf(v->open_hint, sizeof v->open_hint, ML("%s or hold SELECT: this panel"), s_menu.vk_name);
    snprintf(v->h_move, sizeof v->h_move, "%s", ML("Move"));
    snprintf(v->h_adj, sizeof v->h_adj, "%s", ML("Adjust"));
    snprintf(v->h_chg, sizeof v->h_chg, "%s", ML("Change"));
    snprintf(v->h_cat, sizeof v->h_cat, "%s", ML("Category"));
    snprintf(v->h_close, sizeof v->h_close, "%s", ML("Close"));
}

/* -- small anti-aliased drawing into the picture: colour in the DIB (BGRA, straight), coverage alpha in a[] -- */
typedef struct { u8* px; u8* a; int w, h; } ld_mc;
static float mc_c01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static void mc_put(ld_mc* m, int x, int y, unsigned rgb, float s)   /* "over" with source alpha s */
{
    if (s <= 0.002f || x < 0 || y < 0 || x >= m->w || y >= m->h) return;
    if (s > 1.0f) s = 1.0f;
    const size_t i = (size_t)y * (size_t)m->w + (size_t)x;
    u8* p = m->px + i * 4u;
    const float A = (float)m->a[i] * (1.0f / 255.0f), ka = A * (1.0f - s), oa = s + ka, inv = 1.0f / oa;
    p[2] = (u8)(((float)((rgb >> 16) & 255u) * s + (float)p[2] * ka) * inv + 0.5f);
    p[1] = (u8)(((float)((rgb >> 8) & 255u) * s + (float)p[1] * ka) * inv + 0.5f);
    p[0] = (u8)(((float)(rgb & 255u) * s + (float)p[0] * ka) * inv + 0.5f);
    m->a[i] = (u8)(oa * 255.0f + 0.5f);
}
static float mc_rr(float px, float py, float x0, float y0, float x1, float y1, float r)   /* rounded rect: signed distance */
{
    const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
    if (r > hx) r = hx;
    if (r > hy) r = hy;
    const float qx = fabsf(px - (x0 + x1) * 0.5f) - hx + r, qy = fabsf(py - (y0 + y1) * 0.5f) - hy + r;
    const float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f, in = qx > qy ? qx : qy;
    return sqrtf(ox * ox + oy * oy) + (in < 0.0f ? in : 0.0f) - r;
}
/* A rounded rect: filled (line 0) or an inner outline line wide; alpha a at its left to a1 at its right (a1 < 0: a);
 * only the picture's rows cy0 <= y < cy1. */
static void mc_rrect_ex(ld_mc* m, float x0, float y0, float x1, float y1, float r, unsigned rgb, float a, float a1,
                        float line, float cy0, float cy1)
{
    GdiFlush();
    int ix0 = (int)floorf(x0) - 1, iy0 = (int)floorf(y0 > cy0 ? y0 : cy0) - 1;
    int ix1 = (int)ceilf(x1) + 1, iy1 = (int)ceilf(y1 < cy1 ? y1 : cy1) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > m->w) ix1 = m->w;
    if (iy1 > m->h) iy1 = m->h;
    const float span = x1 - x0 > 0.0f ? x1 - x0 : 1.0f;
    for (int y = iy0; y < iy1; y++) {
        const float py = (float)y + 0.5f;
        if (py < cy0 || py >= cy1) continue;
        for (int x = ix0; x < ix1; x++) {
            const float px = (float)x + 0.5f, d = mc_rr(px, py, x0, y0, x1, y1, r);
            float cov = mc_c01(0.5f - d);
            if (line > 0.0f) cov -= mc_c01(0.5f - d - line);
            if (cov <= 0.0f) continue;
            const float aa = a1 >= 0.0f ? a + (a1 - a) * mc_c01((px - x0) / span) : a;
            mc_put(m, x, y, rgb, aa * cov);
        }
    }
}
static void mc_rrect(ld_mc* m, float x0, float y0, float x1, float y1, float r, unsigned rgb, float a)
{
    mc_rrect_ex(m, x0, y0, x1, y1, r, rgb, a, -1.0f, 0.0f, -1e9f, 1e9f);
}
static void mc_ring(ld_mc* m, float x0, float y0, float x1, float y1, float r, unsigned rgb, float a, float line)
{
    mc_rrect_ex(m, x0, y0, x1, y1, r, rgb, a, -1.0f, line, -1e9f, 1e9f);
}
static void mc_disc(ld_mc* m, float cx, float cy, float rad, unsigned rgb, float a)
{
    mc_rrect(m, cx - rad, cy - rad, cx + rad, cy + rad, rad, rgb, a);
}
static void mc_shadow(ld_mc* m, float x0, float y0, float x1, float y1, float r, float spread, float a)
{
    GdiFlush();
    const int ix0 = (int)(x0 - spread) < 0 ? 0 : (int)(x0 - spread), iy0 = (int)(y0 - spread) < 0 ? 0 : (int)(y0 - spread);
    const int ix1 = (int)(x1 + spread) + 1 > m->w ? m->w : (int)(x1 + spread) + 1, iy1 = (int)(y1 + spread) + 1 > m->h ? m->h : (int)(y1 + spread) + 1;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            const float d = mc_rr((float)x + 0.5f, (float)y + 0.5f, x0, y0, x1, y1, r);
            if (d <= 0.0f || d >= spread) continue;
            const float t = 1.0f - d / spread;
            mc_put(m, x, y, 0x000000u, a * t * t);
        }
}
static void mc_line(ld_mc* m, float ax, float ay, float bx, float by, float hw, unsigned rgb, float a)   /* a round-capped stroke */
{
    GdiFlush();
    const float vx = bx - ax, vy = by - ay, ll = vx * vx + vy * vy > 1e-6f ? vx * vx + vy * vy : 1e-6f;
    int ix0 = (int)floorf((ax < bx ? ax : bx) - hw) - 1, iy0 = (int)floorf((ay < by ? ay : by) - hw) - 1;
    int ix1 = (int)ceilf((ax > bx ? ax : bx) + hw) + 1, iy1 = (int)ceilf((ay > by ? ay : by) + hw) + 1;
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > m->w) ix1 = m->w;
    if (iy1 > m->h) iy1 = m->h;
    for (int y = iy0; y < iy1; y++)
        for (int x = ix0; x < ix1; x++) {
            const float px = (float)x + 0.5f - ax, py = (float)y + 0.5f - ay;
            const float t = mc_c01((px * vx + py * vy) / ll), dx = px - t * vx, dy = py - t * vy;
            mc_put(m, x, y, rgb, a * mc_c01(0.5f - (sqrtf(dx * dx + dy * dy) - hw)));
        }
}

/* -- text (GDI on the same DIB: it blends with what the shapes left there) -- */
static int s_menu_cut_logs;
static float mc_text(HDC dc, HFONT f, float x, float ymid, unsigned rgb, const char* utf8, int align, float maxw, int draw)
{   /* align 0 left, 1 right (x = right edge), 2 centred; maxw > 0: cut with an ellipsis (and a log line) when wider.
     * Returns the width. ymid: where the middle of a capital letter sits. */
    wchar_t t[330];
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, t, 330) - 1;
    if (len <= 0) return 0.0f;
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = {0};
    GetTextExtentPoint32W(dc, t, len, &sz);
    if (maxw > 0.0f && (float)sz.cx > maxw) {
        if (draw && s_menu_cut_logs++ < 24) fprintf(stderr, "[menu] text cut to fit %d px (it needs %d): %s\n", (int)maxw, (int)sz.cx, utf8);
        while (len > 1 && (float)sz.cx > maxw) { len--; t[len - 1] = 0x2026; GetTextExtentPoint32W(dc, t, len, &sz); }
    }
    if (draw) {
        TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
        const float em = (float)(tm.tmHeight - tm.tmInternalLeading);
        const int top = (int)floorf(ymid + 0.36f * em - (float)tm.tmAscent + 0.5f);
        const int left = (int)floorf((align == 1 ? x - (float)sz.cx : align == 2 ? x - (float)sz.cx * 0.5f : x) + 0.5f);
        SetTextColor(dc, RGB((rgb >> 16) & 255u, (rgb >> 8) & 255u, rgb & 255u));
        TextOutW(dc, left, top, t, len);
    }
    SelectObject(dc, o);
    return (float)sz.cx;
}

/* the worker's fonts, for one scale */
static struct {
    double k; int icon_ok, cinzel;
    HFONT title, ctitle, cat, row, val, sml, tag, key, hint, icon;
} s_mf;
static HFONT mc_font(double px, int weight, const wchar_t* face)
{
    return CreateFontW(-(int)(px + 0.5), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                       CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}
static void mc_fonts(HDC dc, double k)
{
    if (s_mf.row && s_mf.k == k) return;
    HFONT* all[] = { &s_mf.title, &s_mf.ctitle, &s_mf.cat, &s_mf.row, &s_mf.val, &s_mf.sml, &s_mf.tag, &s_mf.key, &s_mf.hint, &s_mf.icon };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (*all[i]) { DeleteObject(*all[i]); *all[i] = NULL; }
    s_mf.k = k;
    /* the headings in the launcher's Cinzel when it is there (PS3_OVERLAY_FONT), else Segoe UI bold */
    s_mf.title = mc_font(30.0 * k, FW_BLACK, L"Cinzel Black");
    { wchar_t face[64] = L""; HGDIOBJ o = SelectObject(dc, s_mf.title); GetTextFaceW(dc, 64, face); SelectObject(dc, o);
      s_mf.cinzel = !_wcsnicmp(face, L"Cinzel", 6); }
    if (!s_mf.cinzel) { DeleteObject(s_mf.title); s_mf.title = mc_font(28.0 * k, FW_BOLD, L"Segoe UI"); }
    s_mf.ctitle = s_mf.cinzel ? mc_font(21.0 * k, FW_BLACK, L"Cinzel Black") : mc_font(21.0 * k, FW_BOLD, L"Segoe UI");
    s_mf.cat = mc_font(17.0 * k, FW_SEMIBOLD, L"Segoe UI");
    s_mf.row = mc_font(17.0 * k, FW_SEMIBOLD, L"Segoe UI");
    s_mf.val = mc_font(17.0 * k, FW_NORMAL, L"Segoe UI");
    s_mf.sml = mc_font(15.0 * k, FW_NORMAL, L"Segoe UI");
    s_mf.tag = mc_font(11.0 * k, FW_BOLD, L"Segoe UI");
    s_mf.key = mc_font(11.5 * k, FW_SEMIBOLD, L"Segoe UI");
    s_mf.hint = mc_font(13.5 * k, FW_NORMAL, L"Segoe UI");
    s_mf.icon = mc_font(19.0 * k, FW_NORMAL, L"Segoe MDL2 Assets");
    { wchar_t face[64] = L""; HGDIOBJ o = SelectObject(dc, s_mf.icon); GetTextFaceW(dc, 64, face);
      WORD gi[LD_MENU_NCAT]; wchar_t ic[LD_MENU_NCAT];
      for (int c = 0; c < LD_MENU_NCAT; c++) ic[c] = (wchar_t)k_menu_cat[c].icon;
      s_mf.icon_ok = !_wcsicmp(face, L"Segoe MDL2 Assets") &&
                     GetGlyphIndicesW(dc, ic, LD_MENU_NCAT, gi, GGI_MARK_NONEXISTING_GLYPHS) == LD_MENU_NCAT;
      for (int c = 0; s_mf.icon_ok && c < LD_MENU_NCAT; c++) if (gi[c] == 0xFFFF) s_mf.icon_ok = 0;
      SelectObject(dc, o); }
}

/* -- the hint bar's glyphs: each returns its width; draw 0 only measures -- */
typedef struct { ld_mc* m; HDC dc; double k; int draw; float y; } mc_hc;
#define HU(v) ((float)((v) * h->k))
static float mch_pad(mc_hc* h, float x, char sym)   /* a face button: 'x' Cross, 'o' Circle */
{
    if (h->draw) {
        const float cx = x + HU(10), r = HU(10);
        mc_disc(h->m, cx, h->y, r, 0x2A2220u, 0.95f);
        mc_ring(h->m, cx - r, h->y - r, cx + r, h->y + r, r, 0x8A7F76u, 0.9f, HU(1.1));
        if (sym == 'x') {
            const float s = HU(4.2);
            mc_line(h->m, cx - s, h->y - s, cx + s, h->y + s, HU(1.15), 0x9DB8E6u, 1.0f);
            mc_line(h->m, cx - s, h->y + s, cx + s, h->y - s, HU(1.15), 0x9DB8E6u, 1.0f);
        } else {
            const float s = HU(5.0);
            mc_ring(h->m, cx - s, h->y - s, cx + s, h->y + s, s, 0xE58C8Cu, 1.0f, HU(1.8));
        }
    }
    return HU(20);
}
static float mch_xbox(mc_hc* h, float x, const char* letter, unsigned col)   /* an Xbox face button */
{
    if (h->draw) {
        const float cx = x + HU(9), r = HU(9);
        mc_disc(h->m, cx, h->y, r, 0x2A2220u, 0.95f);
        mc_text(h->dc, s_mf.key, cx, h->y, col, letter, 2, 0.0f, 1);
    }
    return HU(18);
}
static float mch_dpad(mc_hc* h, float x, int horiz)
{
    if (h->draw) {
        const float cx = x + HU(10), a = HU(9.5), b = HU(3.4);
        mc_rrect(h->m, cx - b, h->y - a, cx + b, h->y + a, HU(1.5), 0x5A514Bu, 1.0f);
        mc_rrect(h->m, cx - a, h->y - b, cx + a, h->y + b, HU(1.5), 0x5A514Bu, 1.0f);
        if (horiz) { mc_rrect(h->m, cx - a, h->y - b, cx - b, h->y + b, HU(1.5), 0xEDE6DDu, 1.0f);
                     mc_rrect(h->m, cx + b, h->y - b, cx + a, h->y + b, HU(1.5), 0xEDE6DDu, 1.0f); }
        else       { mc_rrect(h->m, cx - b, h->y - a, cx + b, h->y - b, HU(1.5), 0xEDE6DDu, 1.0f);
                     mc_rrect(h->m, cx - b, h->y + b, cx + b, h->y + a, HU(1.5), 0xEDE6DDu, 1.0f); }
    }
    return HU(20);
}
static float mch_cap(mc_hc* h, float x, const char* t, int shoulder)   /* a key cap, or a shoulder button (filled) */
{
    const float tw = mc_text(h->dc, s_mf.key, 0.0f, 0.0f, 0u, t, 0, 0.0f, 0);
    float w = tw + HU(12);
    if (w < HU(shoulder ? 28 : 21)) w = HU(shoulder ? 28 : 21);
    if (h->draw) {
        const float y0 = h->y - HU(shoulder ? 9 : 10), y1 = h->y + HU(shoulder ? 9 : 10);
        mc_rrect(h->m, x, y0, x + w, y1, HU(shoulder ? 6 : 4), shoulder ? 0x2A2220u : 0x1A1514u, 0.95f);
        mc_ring(h->m, x, y0, x + w, y1, HU(shoulder ? 6 : 4), shoulder ? 0x8A7F76u : 0x6E645Eu, 0.9f, HU(1.0));
        mc_text(h->dc, s_mf.key, x + w * 0.5f, h->y, 0xEDE6DDu, t, 2, 0.0f, 1);
    }
    return w;
}
static float mch_label(mc_hc* h, float x, const char* t)
{
    return mc_text(h->dc, s_mf.hint, x, h->y, 0xBFB6AC, t, 0, 0.0f, h->draw);
}
static float mc_hints(mc_hc* h, const ld_menu_view* v, float x)
{
    const float g = HU(4), gl = HU(7), gg = HU(24);
    const float x0 = x;
    x += mch_dpad(h, x, 0) + g; x += mch_cap(h, x, "\xE2\x86\x91\xE2\x86\x93", 0) + gl; x += mch_label(h, x, v->h_move) + gg;
    x += mch_dpad(h, x, 1) + g; x += mch_cap(h, x, "\xE2\x86\x90\xE2\x86\x92", 0) + gl; x += mch_label(h, x, v->h_adj) + gg;
    x += mch_pad(h, x, 'x') + g; x += mch_xbox(h, x, "A", 0x6CC24Au) + g; x += mch_cap(h, x, "Enter", 0) + gl; x += mch_label(h, x, v->h_chg) + gg;
    x += mch_cap(h, x, "L1", 1) + g; x += mch_cap(h, x, "R1", 1) + g; x += mch_cap(h, x, "Q", 0) + g; x += mch_cap(h, x, "E", 0) + gl;
    x += mch_label(h, x, v->h_cat) + gg;
    x += mch_pad(h, x, 'o') + g; x += mch_xbox(h, x, "B", 0xE0484Eu) + g; x += mch_cap(h, x, "Esc", 0) + gl; x += mch_label(h, x, v->h_close);
    return x - x0;
}
#undef HU

/* The picture of view v, premultiplied RGBA in a new upload buffer (row pitch *pitch). The worker thread. */
static int ld_menu_paint(const ld_menu_view* v, ID3D12Resource** up_out, u32* w_out, u32* h_out, u32* pitch_out, const char* dump)
{
    const double PW0 = 960.0, PH0 = 444.0, M0 = 18.0;   /* the panel at 720 lines, and the room for its shadow */
    double k = (double)v->bh / 720.0;
    if ((PW0 + 2.0 * M0) * k > (double)v->bw - 8.0) k = ((double)v->bw - 8.0) / (PW0 + 2.0 * M0);   /* a narrow window */
    if (k < 0.5) k = 0.5;
#define U(x) ((float)((x) * k))
    const int W = (int)ceil((PW0 + 2.0 * M0) * k), H = (int)ceil((PH0 + 2.0 * M0) * k);
    if (!s_menu.font_added) {   /* the launcher's heading font (Cinzel), private to this process */
        s_menu.font_added = 1;
        wchar_t e[MAX_PATH]; const DWORD n = GetEnvironmentVariableW(L"PS3_OVERLAY_FONT", e, MAX_PATH);
        if (n > 0 && n < MAX_PATH && !AddFontResourceExW(e, FR_PRIVATE, NULL)) fprintf(stderr, "[menu] heading font not loaded (PS3_OVERLAY_FONT)\n");
    }
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    u8* alpha = (u8*)calloc((size_t)W * (size_t)H, 1);
    if (!bmp || !bits || !alpha) { if (bmp) DeleteObject(bmp); free(alpha); DeleteDC(dc); return 0; }
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)W * (size_t)H * 4u);
    SetBkMode(dc, TRANSPARENT);
    mc_fonts(dc, k);
    ld_mc mm = { (u8*)bits, alpha, W, H }; ld_mc* m = &mm;
    /* colours: the launcher's (launcher/app/ui.py) */
    const unsigned c_text = 0xEDE6DDu, c_dim = 0xCFC6BCu, c_grey = 0x7E7570u, c_gold = 0xE2B86Au, c_gold2 = 0xF2C879u,
                   c_red = 0xA4161Au, c_red2 = 0xC8282Eu, c_line = 0x3A2E2Au;
    const float X = U(M0), Y = U(M0), PW = U(PW0), PH = U(PH0), R = U(14);

    /* the panel: shadow, translucent body, a warm wash behind the title, a thin bronze edge */
    mc_shadow(m, X, Y, X + PW, Y + PH, R, U(M0), 0.55f);
    mc_rrect(m, X, Y, X + PW, Y + PH, R, 0x110D0Cu, 0.90f);
    mc_rrect_ex(m, X, Y, X + PW, Y + PH, R, 0x4A1014u, 0.55f, 0.0f, 0.0f, Y, Y + U(66));
    mc_rrect_ex(m, X, Y + PH - U(48), X + PW, Y + PH, R, 0x070505u, 0.60f, -1.0f, 0.0f, Y + PH - U(48), 1e9f);   /* hint bar */
    mc_ring(m, X, Y, X + PW, Y + PH, R, 0x7A5C38u, 0.60f, U(1.2));

    /* header: title, the game's state, saving */
    mc_text(dc, s_mf.title, X + U(30), Y + U(34), c_gold, v->title, 0, U(440), 1);
    mc_rrect_ex(m, X + U(24), Y + U(65), X + PW - U(24), Y + U(66.5), 0.0f, 0xB3202Au, 0.95f, 0.0f, 0.0f, -1e9f, 1e9f);
    { const float tw = mc_text(dc, s_mf.tag, 0, 0, 0, v->pill, 0, 0.0f, 0);
      const float x1 = X + PW - U(28), x0 = x1 - tw - U(32), y0 = Y + U(17), y1 = y0 + U(24);
      const unsigned fc = v->paused ? c_gold : 0xB3202Au, sc = v->paused ? c_gold : 0xD8343Bu, tc = v->paused ? 0xF0D49Au : 0xF6D2D2u;
      mc_rrect(m, x0, y0, x1, y1, U(12), fc, v->paused ? 0.16f : 0.30f);
      mc_ring(m, x0, y0, x1, y1, U(12), sc, 0.85f, U(1.1));
      mc_disc(m, x0 + U(13), (y0 + y1) * 0.5f, U(3.5), v->paused ? c_gold2 : 0xFF4A50u, 1.0f);
      mc_text(dc, s_mf.tag, x0 + U(22), (y0 + y1) * 0.5f, tc, v->pill, 0, 0.0f, 1);
      mc_text(dc, s_mf.hint, x1, Y + U(52), v->save_bad ? 0xE0565Cu : 0x8A817Au, v->save, 1, U(420), 1); }

    /* left: the categories */
    mc_rrect(m, X + U(12), Y + U(78), X + U(236), Y + PH - U(60), U(10), 0x080606u, 0.55f);
    for (int c = 0; c < LD_MENU_NCAT; c++) {
        const float x0 = X + U(20), x1 = X + U(228), y0 = Y + U(90) + (float)c * U(52), y1 = y0 + U(44), ym = (y0 + y1) * 0.5f;
        const int sel = c == v->cat_sel;
        if (sel) {
            mc_rrect_ex(m, x0, y0, x1, y1, U(8), c_red, 0.98f, 0.78f, 0.0f, -1e9f, 1e9f);
            mc_rrect(m, x0, y0 + U(9), x0 + U(3), y1 - U(9), U(1.5), c_gold, 1.0f);
        }
        float tx = x0 + U(16);
        if (s_mf.icon_ok) {
            char ic[8]; const wchar_t w1[2] = { (wchar_t)k_menu_cat[c].icon, 0 };
            WideCharToMultiByte(CP_UTF8, 0, w1, -1, ic, sizeof ic, NULL, NULL);
            mc_text(dc, s_mf.icon, x0 + U(26), ym, sel ? 0xFFFFFFu : 0x9C8F84u, ic, 2, 0.0f, 1);
            tx = x0 + U(48);
        }
        mc_text(dc, s_mf.cat, tx, ym, sel ? 0xFFFFFFu : c_dim, v->cat[c], 0, x1 - tx - U(8), 1);
    }

    mc_text(dc, s_mf.hint, X + U(26), Y + PH - U(80), 0x8A817Au, v->open_hint, 0, U(200), 1);

    /* right: the chosen category's options */
    const float rx0 = X + U(256), rx1 = X + PW - U(28);
    mc_text(dc, s_mf.ctitle, rx0 + U(2), Y + U(100), c_gold, v->cat[v->cat_sel], 0, rx1 - rx0, 1);
    mc_rrect(m, rx0, Y + U(122), rx1, Y + U(123), 0.0f, c_line, 1.0f);
    for (int i = 0; i < v->nrow; i++) {
        const ld_menu_vrow* o = &v->row[i];
        const float y0 = Y + U(136) + (float)i * U(62), y1 = y0 + U(54), ym = (y0 + y1) * 0.5f;
        const int sel = i == v->row_sel;
        if (sel) {
            mc_rrect_ex(m, rx0, y0, rx1, y1, U(8), c_red, 0.92f, 0.50f, 0.0f, -1e9f, 1e9f);
            mc_rrect(m, rx0, y0 + U(10), rx0 + U(3), y1 - U(10), U(1.5), c_gold, 1.0f);
        } else mc_rrect(m, rx0, y0, rx1, y1, U(8), 0x1C1615u, 0.70f);
        const unsigned lc = o->grey ? c_grey : sel ? 0xFFFFFFu : c_text;
        mc_text(dc, s_mf.row, rx0 + U(18), ym, lc, o->label, 0, U(230), 1);
        /* the tag: LIVE / AFTER RESTART / SWITCHING */
        const float tg1 = rx1 - U(14);
        if (o->tagk) {
            const float tw = mc_text(dc, s_mf.tag, 0, 0, 0, o->tag, 0, 0.0f, 0);
            float tw2 = tw + U(16); if (tw2 > U(124)) tw2 = U(124);
            const float t0 = tg1 - tw2, ty0 = ym - U(10), ty1 = ym + U(10);
            if (o->tagk == 2) mc_rrect(m, t0, ty0, tg1, ty1, U(10), 0x6E4E22u, 0.95f);
            else { mc_rrect(m, t0, ty0, tg1, ty1, U(10), 0x000000u, 0.25f);
                   mc_ring(m, t0, ty0, tg1, ty1, U(10), o->tagk == 1 ? 0xC9A15Au : 0xFFFFFFu, 0.75f, U(1.0)); }
            mc_text(dc, s_mf.tag, (t0 + tg1) * 0.5f, ym, o->tagk == 2 ? 0xF6E7C8u : o->tagk == 1 ? c_gold : 0xFFFFFFu, o->tag, 2, tw2 - U(10), 1);
        }
        /* the control */
        const float cx1 = rx1 - U(14 + 124 + 16), cx0 = cx1 - U(262);
        const unsigned vc = o->grey ? c_grey : sel ? 0xFFFFFFu : c_text;
        if (o->kind == MKIND_SLIDER) {
            const float t0 = cx0 + U(10), t1 = cx1 - U(92), xk = t0 + (t1 - t0) * o->frac;
            mc_rrect(m, t0, ym - U(2), t1, ym + U(2), U(2), 0x3A302Cu, 1.0f);
            if (o->frac > 0.0f) mc_rrect(m, t0, ym - U(2), xk, ym + U(2), U(2), o->grey ? c_grey : sel ? c_gold : c_red2, 1.0f);
            for (int t = 0; t < o->nticks; t++) {
                const float xt = t0 + (t1 - t0) * o->tick[t];
                mc_disc(m, xt, ym, U(2.2), o->tick[t] <= o->frac + 1e-3f ? (sel ? 0xFFF1D0u : 0xE8B0B0u) : 0x6A5E58u, 1.0f);
            }
            mc_disc(m, xk, ym, U(8.5), 0xF4EEE6u, o->grey ? 0.5f : 1.0f);
            mc_ring(m, xk - U(8.5), ym - U(8.5), xk + U(8.5), ym + U(8.5), U(8.5), sel ? c_gold : c_red, 1.0f, U(2.0));
            mc_text(dc, s_mf.val, cx1, ym, vc, o->val, 1, U(84), 1);
        } else if (o->kind == MKIND_TOGGLE) {
            const float s1 = cx1, s0 = s1 - U(48), sy0 = ym - U(12), sy1 = ym + U(12), ga = o->grey ? 0.45f : 1.0f;
            if (o->on) {
                mc_rrect(m, s0, sy0, s1, sy1, U(12), sel ? 0xD8343Bu : c_red2, ga);
                mc_disc(m, s1 - U(12), ym, U(9), 0xFFFFFFu, ga);
            } else {
                mc_rrect(m, s0, sy0, s1, sy1, U(12), 0x2C2422u, ga);
                mc_ring(m, s0, sy0, s1, sy1, U(12), 0x6A5E58u, ga, U(1.2));
                mc_disc(m, s0 + U(12), ym, U(8), 0xA89C94u, ga);
            }
            mc_text(dc, s_mf.val, s0 - U(14), ym, vc, o->val, 1, U(210), 1);
        } else {
            if (!o->grey) {
                const unsigned ac = sel ? c_gold2 : 0x6A605Au;
                const float lx = cx0 + U(10), rx = cx1 - U(10), s = U(6.5), hw = U(1.4);
                mc_line(m, lx + s * 0.75f, ym - s, lx, ym, hw, ac, 1.0f); mc_line(m, lx, ym, lx + s * 0.75f, ym + s, hw, ac, 1.0f);
                mc_line(m, rx - s * 0.75f, ym - s, rx, ym, hw, ac, 1.0f); mc_line(m, rx, ym, rx - s * 0.75f, ym + s, hw, ac, 1.0f);
            }
            mc_text(dc, s_mf.val, (cx0 + cx1) * 0.5f, ym, vc, o->val, 2, U(262 - 52), 1);
        }
    }
    /* the "Active now" card (categories with two options) */
    if (v->ncard && v->nrow < 3) {
        const float y0 = Y + U(136) + (float)v->nrow * U(62) + U(4), y1 = y0 + U(72);
        mc_rrect(m, rx0, y0, rx1, y1, U(8), 0x0A0707u, 0.55f);
        mc_ring(m, rx0, y0, rx1, y1, U(8), 0x4A3A30u, 0.80f, U(1.0));
        mc_text(dc, s_mf.tag, rx0 + U(18), y0 + U(16), c_gold, v->card_title, 0, U(300), 1);
        const float cw = (rx1 - rx0 - U(36)) / (float)v->ncard;
        for (int c = 0; c < v->ncard; c++) {
            const float cx = rx0 + U(18) + (float)c * cw;
            if (c) mc_rrect(m, cx - U(10), y0 + U(30), cx - U(9), y1 - U(12), 0.0f, 0x3A2E2Au, 1.0f);
            mc_text(dc, s_mf.hint, cx, y0 + U(38), 0x9C8F84u, v->card_k[c], 0, cw - U(20), 1);
            mc_text(dc, s_mf.val, cx, y0 + U(57), c_text, v->card_v[c], 0, cw - U(20), 1);
        }
    }
    /* the highlighted option in one line */
    { const float ym = Y + PH - U(78);
      mc_rrect(m, rx0, Y + PH - U(102), rx1, Y + PH - U(101), 0.0f, c_line, 1.0f);
      if (v->desc[0]) {
          mc_rrect(m, rx0 + U(2), ym - U(8), rx0 + U(5), ym + U(8), U(1.5), v->desc_warn ? 0xEFA85Au : c_gold, 1.0f);
          mc_text(dc, s_mf.sml, rx0 + U(16), ym, v->desc_warn ? 0xEFA85Au : 0xD9D1C7u, v->desc, 0, rx1 - rx0 - U(16), 1);
      } }
    /* the hint bar, centred */
    { mc_hc hc = { m, dc, k, 0, Y + PH - U(24) };
      const float w = mc_hints(&hc, v, 0.0f), room = PW - U(40);
      if (w > room && s_menu_cut_logs++ < 24) fprintf(stderr, "[menu] the hint bar is %d px for %d px of room\n", (int)w, (int)room);
      hc.draw = 1;
      mc_hints(&hc, v, X + (PW - (w < room ? w : room)) * 0.5f); }
    GdiFlush();
#undef U
    /* premultiplied RGBA into a new upload buffer */
    int ok = 0;
    const u32 pitch = ((u32)W * 4u + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd = {0};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (u64)pitch * (u64)H; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* up = NULL; u8* map = NULL;
    D3D12_RANGE none = {0, 0};
    if (g.dev && SUCCEEDED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&up))) {
        if (SUCCEEDED(up->lpVtbl->Map(up, 0, &none, (void**)&map))) {
            for (int y = 0; y < H; y++) {
                u8* o = map + (size_t)y * pitch;
                const u8* p = (const u8*)bits + (size_t)y * (size_t)W * 4u;
                const u8* a = alpha + (size_t)y * (size_t)W;
                for (int x = 0; x < W; x++, o += 4, p += 4) {
                    const u32 al = a[x];
                    o[0] = (u8)((p[2] * al + 127u) / 255u); o[1] = (u8)((p[1] * al + 127u) / 255u);
                    o[2] = (u8)((p[0] * al + 127u) / 255u); o[3] = (u8)al;
                }
            }
            if (dump) {   /* tests: premultiplied colour + alpha, as plain files */
                char pa[MAX_PATH * 2];
                snprintf(pa, sizeof pa, "%s.ppm", dump);
                FILE* f = fopen(pa, "wb");
                snprintf(pa, sizeof pa, "%s_a.pgm", dump);
                FILE* fa = fopen(pa, "wb");
                u8* row = (u8*)malloc((size_t)W * 4u);
                if (f && fa && row) {
                    fprintf(f, "P6\n%d %d\n255\n", W, H); fprintf(fa, "P5\n%d %d\n255\n", W, H);
                    for (int y = 0; y < H; y++) {
                        const u8* q = map + (size_t)y * pitch;
                        for (int x = 0; x < W; x++) { row[x * 3] = q[x * 4]; row[x * 3 + 1] = q[x * 4 + 1]; row[x * 3 + 2] = q[x * 4 + 2]; }
                        fwrite(row, 3, (size_t)W, f);
                        for (int x = 0; x < W; x++) row[x] = q[x * 4 + 3];
                        fwrite(row, 1, (size_t)W, fa);
                    }
                }
                free(row);
                if (f) fclose(f);
                if (fa) fclose(fa);
            }
            up->lpVtbl->Unmap(up, 0, NULL);
            ok = 1;
        } else { up->lpVtbl->Release(up); up = NULL; }
    } else up = NULL;
    SelectObject(dc, old_bmp);
    DeleteObject(bmp); DeleteDC(dc); free(alpha);
    *up_out = up; *w_out = (u32)W; *h_out = (u32)H; *pitch_out = pitch;
    return ok;
}

/* -- the worker: makes the newest asked-for picture -- */
static struct {
    HANDLE th, ev; CRITICAL_SECTION cs; int st;   /* st: 0 not started, 1 running, -1 no thread (pictures made inline) */
    ld_menu_view want; int want_set;
    ID3D12Resource* done; u32 w, h, pitch, bw, bh; unsigned seq; double ms; int fails;
} s_mw;
static void ld_menu_make(const ld_menu_view* v)
{
    const unsigned long long t0 = fsr3_now_us();
    ID3D12Resource* up = NULL; u32 w = 0, h = 0, pitch = 0;
    const int ok = ld_menu_paint(v, &up, &w, &h, &pitch, NULL);
    const double ms = (double)(fsr3_now_us() - t0) / 1000.0;
    { const char* d = v->dump ? getenv("LD_MENU_DUMP") : NULL;   /* tests: the same view at other output sizes */
      const char* sz = getenv("LD_MENU_DUMP_SIZES");
      if (!sz || !sz[0]) sz = "1920x1080,2560x1440,3840x2160";
      for (const char* q = d && d[0] ? sz : NULL; q && *q; ) {
          unsigned dw = 0, dh = 0;
          if (sscanf(q, "%ux%u", &dw, &dh) == 2 && dw >= 320 && dh >= 180 && dw <= 8192 && dh <= 8192) {
              ld_menu_view t = *v; t.bw = dw; t.bh = dh;
              char path[MAX_PATH * 2]; snprintf(path, sizeof path, "%s\\menu_%02d_%ux%u", d, v->dump, dw, dh);
              ID3D12Resource* u2 = NULL; u32 w2, h2, p2;
              const unsigned long long t1 = fsr3_now_us();
              const int ok2 = ld_menu_paint(&t, &u2, &w2, &h2, &p2, path);
              if (u2) u2->lpVtbl->Release(u2);
              fprintf(stderr, "[menu] test picture %s.ppm: %ux%u for %ux%u, %s, %.1f ms\n", path, w2, h2, dw, dh, ok2 ? "written" : "FAILED",
                      (double)(fsr3_now_us() - t1) / 1000.0);
          }
          q = strchr(q, ',');
          if (q) q++;
      } }
    if (s_mw.st > 0) EnterCriticalSection(&s_mw.cs);
    if (s_mw.done) s_mw.done->lpVtbl->Release(s_mw.done);   /* a picture never taken: never handed to the GPU */
    s_mw.done = ok ? up : NULL;
    if (!ok) { if (up) up->lpVtbl->Release(up); s_mw.fails++; }
    s_mw.w = w; s_mw.h = h; s_mw.pitch = pitch; s_mw.bw = v->bw; s_mw.bh = v->bh; s_mw.seq = v->seq; s_mw.ms = ms;
    if (s_mw.st > 0) LeaveCriticalSection(&s_mw.cs);
}
static DWORD WINAPI ld_menu_worker(LPVOID arg)
{
    (void)arg;
    static ld_menu_view v;
    for (;;) {
        WaitForSingleObject(s_mw.ev, INFINITE);
        int have = 0;
        EnterCriticalSection(&s_mw.cs);
        if (s_mw.want_set) { v = s_mw.want; s_mw.want_set = 0; have = 1; }
        LeaveCriticalSection(&s_mw.cs);
        if (have) ld_menu_make(&v);
    }
    return 0;
}
static void ld_menu_request(u32 bw, u32 bh)
{
    static unsigned seq;
    static ld_menu_view v;
    ld_menu_view_make(&v, bw, bh);
    v.seq = ++seq;
    s_menu.req_bw = bw; s_menu.req_bh = bh;
    if (s_mw.st == 0) {
        InitializeCriticalSection(&s_mw.cs);
        s_mw.ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        s_mw.th = s_mw.ev ? CreateThread(NULL, 0, ld_menu_worker, NULL, 0, NULL) : NULL;
        s_mw.st = s_mw.th ? 1 : -1;
        if (s_mw.st < 0) fprintf(stderr, "[menu] no worker thread: the panel's pictures are made in the frame\n");
    }
    if (s_mw.st < 0) { ld_menu_make(&v); return; }
    EnterCriticalSection(&s_mw.cs);
    s_mw.want = v; s_mw.want_set = 1;
    LeaveCriticalSection(&s_mw.cs);
    SetEvent(s_mw.ev);
}
/* A finished picture: into a fresh texture (the copy is recorded on this frame's list). The old texture and upload
 * buffer may still feed a frame the GPU has not run: they are retired, never rewritten (as the credit line). */
static void ld_menu_take(void)
{
    if (s_mw.st == 0) return;
    ID3D12Resource* up; u32 w, h, pitch, bw, bh; double ms;
    if (s_mw.st > 0) EnterCriticalSection(&s_mw.cs);
    up = s_mw.done; s_mw.done = NULL; w = s_mw.w; h = s_mw.h; pitch = s_mw.pitch; bw = s_mw.bw; bh = s_mw.bh; ms = s_mw.ms;
    if (s_mw.st > 0) LeaveCriticalSection(&s_mw.cs);
    if (!up) return;
    D3D12_HEAP_PROPERTIES hp = {0}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {0};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count = 1;
    ID3D12Resource* tex = NULL;
    if (FAILED(g.dev->lpVtbl->CreateCommittedResource(g.dev, &hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void**)&tex))) { up->lpVtbl->Release(up); return; }
    if (s_menu.up) { retire_texture(s_menu.up); s_menu.up = NULL; }
    if (s_menu.tex) { retire_texture(s_menu.tex); s_menu.tex = NULL; }
    s_menu.up = up; s_menu.tex = tex;
    D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
    src.pResource = up; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    src.PlacedFootprint.Footprint.Width = w; src.PlacedFootprint.Footprint.Height = h;
    src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = pitch;
    dst.pResource = tex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
    LD_CL(CopyTextureRegion, &dst, 0, 0, 0, &src, NULL);
    ld_pp_barrier(tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    srv_write(SRV_MENU, tex);
    s_menu.w = w; s_menu.h = h; s_menu.tex_bw = bw; s_menu.tex_bh = bh;
    static unsigned logs;
    if (logs++ < 6) fprintf(stderr, "[menu] panel picture %ux%u for %ux%u made in %.1f ms %s\n", w, h, bw, bh, ms,
                            s_mw.st > 0 ? "on the worker thread" : "in the frame");
}

/* ---- ovl3: the overlay's own layer under frame generation --------------------------------------------------------- */
/* With FSR frame generation the panel and the FPS counter must not be in the frame FSR interpolates from (they would
 * smear / ghost on every generated frame: tester report, HANDOFF 2026-11-11). On a frame that gets a generated frame
 * they are drawn into their own texture (LD_PP_UI: swap-chain size, cleared to transparent, premultiplied alpha) and
 * that texture is handed to AMD's swap chain as its UI layer (fsr3_fg_ui), which composes it onto the real and the
 * generated frame. Every other frame (no generation, movies, FSR off) draws them into the frame as before.
 *   LD_FG_UI=0   no layer: the old way (generation rests while the panel is open; the FPS counter is in the frame) */
static struct {
    int st;                             /* 0 not looked at, 1 usable, -1 off (LD_FG_UI=0 or its texture failed) */
    unsigned long long cand, begun;     /* the frame (s_fsr3.frame_id + 1) that may use / uses the layer */
    D3D12_RESOURCE_STATES rs; ID3D12Resource* res;   /* the layer's tracked state and texture */
    unsigned long long frames;          /* frames that used it (log) */
} s_fgui;
static int ld_fgui_usable(void)
{
    if (s_fgui.st == 0) { const char* e = getenv("LD_FG_UI"); s_fgui.st = e && e[0] == '0' ? -1 : 1;
                          if (s_fgui.st < 0) fprintf(stderr, "[menu] LD_FG_UI=0: overlay drawn into the frame under frame generation\n"); }
    return s_fgui.st > 0 && s_fsr3.on && !s_fsr3.ui_fail && g.bb_w == s_fsr3.w && g.bb_h == s_fsr3.h;
}
static int ld_fgui_on(void) { return s_fgui.begun && s_fgui.begun == s_fsr3.frame_id + 1; }
static void ld_fgui_to(D3D12_RESOURCE_STATES want)
{
    if (s_fgui.res && s_fgui.rs != want) { ld_pp_barrier(s_fgui.res, s_fgui.rs, want); s_fgui.rs = want; }
}
/* A game frame's present pass starts (game: not the loading turbo): may its overlay go into the layer? */
static void ld_fgui_frame(int game)
{
    s_fgui.cand = 0;
    if (!s_fsr3.on || !ld_fgui_usable()) return;
    if (fsr3_fg_decide(game)) s_fgui.cand = s_fsr3.frame_id + 1;
}
/* Something is about to be drawn into the overlay: 1 when it goes into the layer (begun, in RENDER_TARGET). */
static int ld_fgui_begin(void)
{
    if (ld_fgui_on()) { ld_fgui_to(D3D12_RESOURCE_STATE_RENDER_TARGET); return 1; }
    if (!s_fgui.cand || s_fgui.cand != s_fsr3.frame_id + 1 || !ld_fgui_usable()) return 0;
    if (!ld_pp_init() || !ld_pp_target(LD_PP_UI, g.bb_w, g.bb_h, DXGI_FORMAT_R8G8B8A8_UNORM) || !ld_pp_copy_pso(DXGI_FORMAT_R8G8B8A8_UNORM)) {
        s_fgui.st = -1; fprintf(stderr, "[menu] the overlay's layer could not be made: overlay drawn into the frame\n"); return 0; }
    if (s_fgui.res != s_pp.t[LD_PP_UI].res) { s_fgui.res = s_pp.t[LD_PP_UI].res; s_fgui.rs = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; }
    ld_fgui_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    LD_CL(ClearRenderTargetView, rtv_handle(LD_RTV_PP + LD_PP_UI), clear, 0, NULL);
    s_fgui.begun = s_fsr3.frame_id + 1;
    if (s_fgui.frames++ == 0) fprintf(stderr, "[menu] overlay layer in use under frame generation (%ux%u): panel and FPS counter stay out of the generated frames\n", g.bb_w, g.bb_h);
    return 1;
}
/* In place of ld_perf_overlay(bb) (bb in COPY_DEST): the FPS counter into the layer on a frame with generation; then
 * the layer, when this frame used it, goes to the swap chain (before_present). */
static void ld_fgui_perf(ID3D12Resource* bb)
{
    const char* e = s_ld_perf_on < 0 ? getenv("LD_PERF_OVERLAY") : NULL;
    const int draws = s_ld_perf_draw && (s_ld_perf_on > 0 || (s_ld_perf_on < 0 && e && e[0] && e[0] != '0'));
    if (draws && ld_fgui_begin()) {
        ld_fgui_to(D3D12_RESOURCE_STATE_COPY_DEST);
        ld_perf_overlay(s_fgui.res);
    } else ld_perf_overlay(bb);
    if (ld_fgui_on()) {
        ld_fgui_to(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        s_fsr3.ui_res = s_fgui.res; s_fsr3.ui_stamp = s_fgui.begun;
    }
}
/* LD_FRAME_DUMP beside frame_<tag>_window.ppm (the frame FSR interpolates from): frame_<tag>_ui.ppm, the layer
 * (premultiplied colour over black; only on frames that used it). */
static void ld_fgui_dump(const char* dir, unsigned tag)
{
    if (!ld_fgui_on() || !s_fgui.res) return;
    surface_t o; memset(&o, 0, sizeof o);
    o.tex = s_fgui.res; o.w = g.bb_w; o.h = g.bb_h; o.fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    char path[MAX_PATH * 2];
    snprintf(path, sizeof(path), "%s\\frame_%06u_ui.ppm", dir, tag);
    ld_fgui_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
    ld_dump_surface_ppm(path, &o);
    ld_fgui_to(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

/* The fade-out time (ovl4: 120 ms; the fade in is 4/3 of it). LD_MENU_FADE_MS=<ms> (tests: 0..5000). */
static unsigned long long ld_menu_fade_us(void)
{
    static long long us = -1;
    if (us < 0) { const char* e = getenv("LD_MENU_FADE_MS"); const int ms = e && e[0] ? atoi(e) : 120;
                  us = (ms < 0 ? 0 : ms > 5000 ? 5000 : ms) * 1000ll; }
    return (unsigned long long)us;
}
/* The panel is fading out after closing. */
static int ld_menu_fading(void)
{
    return !s_menu.open && s_menu.close_us && s_menu.tex && fsr3_now_us() - s_menu.close_us < ld_menu_fade_us() + 5000ull;
}
/* The panel over the frame's rect (0, 0, dw, dh) of swap-chain buffer bbi, which is in RENDER_TARGET. */
static void ld_menu_draw(u32 bbi, u32 dw, u32 dh)
{
    if (dw < 320u || dh < 180u) return;
    if (s_menu.open) {
        const int p = cellPad_menu_game_paused();
        if (p != s_menu.paused_shown) s_menu.dirty = 1;
        if (s_menu.dirty || s_menu.req_bw != g.bb_w || s_menu.req_bh != g.bb_h) { ld_menu_request(g.bb_w, g.bb_h); s_menu.dirty = 0; }
    }
    ld_menu_take();
    if (!s_menu.tex || s_menu.tex_bw != g.bb_w || s_menu.tex_bh != g.bb_h) return;   /* this size's picture is not made yet */
    if (s_menu.w > dw || s_menu.h > dh) return;
    /* open: 160 ms fade in + a short slide up; close: 120 ms fade out (from where the fade in was) */
    const unsigned long long now = fsr3_now_us();
    float e;
    if (s_menu.open) {
        if (!s_menu.anim_us) s_menu.anim_us = now;   /* it starts with the first picture */
        const float t = ld_menu_fade_us() ? mc_c01((float)(now - s_menu.anim_us) / ((float)ld_menu_fade_us() * 4.0f / 3.0f)) : 1.0f;
        e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        s_menu.fade_now = e;
    } else {
        const float t = ld_menu_fade_us() ? mc_c01((float)(now - s_menu.close_us) / (float)ld_menu_fade_us()) : 1.0f;
        e = s_menu.fade_close * (1.0f - t) * (1.0f - t);
        if (e <= 0.004f) return;
    }
    const u32 slide = (u32)((1.0f - e) * 14.0f * (float)g.bb_h / 720.0f + 0.5f);
    u32 y = (dh - s_menu.h) / 2u + slide;
    if (y + s_menu.h > dh) y = dh - s_menu.h;
    const float c[8] = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, e, 0.0f };   /* whole texture, point, the fade */
    D3D12_GPU_VIRTUAL_ADDRESS va;
    if (!ld_pp_room(c, 2, 1, 0, &va)) return;                /* rings full: no flush here, skip a frame */
    const D3D12_GPU_DESCRIPTOR_HANDLE table = ld_pp_table(SRV_MENU, SRV_WHITE, SRV_WHITE);
    if (!table.ptr) return;
    /* frame generation: into the overlay's layer (the premultiplied panel copied onto its transparent clear: the layer
     * then holds the panel's own alpha); else blended over the frame */
    if (ld_fgui_begin())
        ld_pp_draw(s_menu.pso_plain, table, va, rtv_handle(LD_RTV_PP + LD_PP_UI), (dw - s_menu.w) / 2u, y, s_menu.w, s_menu.h);
    else
        ld_pp_draw(s_menu.pso, table, va, rtv_handle(bbi), (dw - s_menu.w) / 2u, y, s_menu.w, s_menu.h);
}

#include "rsx_trophy_toast.inc"   /* host/trophies: the trophy popup */

/* ---- opening, closing, input --------------------------------------------------------------------------------------- */
static int ld_menu_focused(void)
{
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return 1;
    static HWND host = (HWND)-1;   /* rendering inside the launcher's window: that window in front counts */
    if (host == (HWND)-1) {
        const char* e = getenv("PS3_PARENT_HWND");
        HWND p = e && *e ? (HWND)(uintptr_t)strtoull(e, NULL, 0) : NULL;
        host = p ? GetAncestor(p, GA_ROOT) : NULL;
    }
    return host && fg == host;
}
enum { MN_UP = 1, MN_DOWN = 2, MN_LEFT = 4, MN_RIGHT = 8, MN_OK = 16, MN_BACK = 32, MN_ESC = 64, MN_KEY = 128, MN_SELECT = 256,
       MN_L1 = 512, MN_R1 = 1024 };
static unsigned ld_menu_nav(void)
{
    unsigned n = 0;
    if (s_menu.n_ev) return 0;   /* a test script drives the panel: the pad and the keys are not looked at */
    unsigned b; int lx, ly;
    cellPad_menu_read(&b, &lx, &ly);
    if ((b & 0x0010u) || ly < 64) n |= MN_UP;       /* CELL_PAD_CTRL_UP / the left stick */
    if ((b & 0x0040u) || ly > 192) n |= MN_DOWN;
    if ((b & 0x0080u) || lx < 64) n |= MN_LEFT;
    if ((b & 0x0020u) || lx > 192) n |= MN_RIGHT;
    if (b & 0x4000u) n |= MN_OK;                    /* CROSS */
    if (b & 0x2000u) n |= MN_BACK;                  /* CIRCLE */
    if (b == 0x0001u) n |= MN_SELECT;               /* SELECT alone */
    if (b & 0x0400u) n |= MN_L1;                    /* L1 / R1: the category */
    if (b & 0x0800u) n |= MN_R1;
    if (ld_menu_focused()) {
        if (GetAsyncKeyState(VK_UP) & 0x8000) n |= MN_UP;
        if (GetAsyncKeyState(VK_DOWN) & 0x8000) n |= MN_DOWN;
        if (GetAsyncKeyState(VK_LEFT) & 0x8000) n |= MN_LEFT;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) n |= MN_RIGHT;
        if (GetAsyncKeyState(VK_RETURN) & 0x8000) n |= MN_OK;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) n |= MN_ESC;
        if ((GetAsyncKeyState('Q') & 0x8000) || (GetAsyncKeyState(VK_PRIOR) & 0x8000)) n |= MN_L1;
        if ((GetAsyncKeyState('E') & 0x8000) || (GetAsyncKeyState(VK_NEXT) & 0x8000)) n |= MN_R1;
        if (GetAsyncKeyState(s_menu.vk) & 0x8000) n |= MN_KEY;
    }
    return n;
}
static void ld_menu_open(const char* how)
{
    if (s_menu.open) return;
    s_menu.open = 1; s_menu.dirty = 1; s_menu.opened++;
    s_menu.anim_us = 0; s_menu.close_us = 0; s_menu.fade_now = 0.0f;   /* the fade starts with the first picture */
    cellPad_menu_block(1);
    g_rsx_menu_owns_esc = 1;
    s_menu.nav_prev = ~0u; s_menu.rep_bit = 0; s_menu.sel_armed = 0; s_menu.sel_since = 0;   /* nothing held counts until let go */
    s_menu.open_ms = GetTickCount64();
    long long wr, ps; long bk;
    cellPad_menu_counters(&s_menu.c_neutral0, &s_menu.c_input0, &wr, &ps, &s_menu.c_passed_in0, &bk);
    s_menu.gen0 = s_fsr3.tot_gen; s_menu.off0 = s_fsr3.tot_off;
    { long long a, b, c, d; long lp; cellPad_menu_select_counters(&a, &b, &c, &d, &lp, &s_menu.pause_reads0); }
    fprintf(stderr, "[menu] opened by %s at frame %u (game paused: %s)\n", how, g_ld_frames,
            cellPad_menu_game_paused() == 1 ? "yes" : cellPad_menu_game_paused() == 0 ? "no" : "unknown");
    /* Frame generation rests while the panel is on screen (s_fsr3.menu_hold). A game started with frame generation
     * can still have its limiter at the limit from before (60) and hold FSR's pace only by waiting on the screen:
     * without generated frames it would then jump to that limit (review fault 3). So the limiter takes FSR's pace
     * here, as ld_menu_fg(1) does; it stays after closing (the same pace generation wants). */
    if (ld_menu_fsr_live() == 2 && s_fsr3.fg_cap > 0) {
        const double uc = ld_menu_user_cap();
        const double cap = uc <= 0 || uc > s_fsr3.fg_cap + 0.01 ? s_fsr3.fg_cap : uc;
        const double have = cellGcm_flip_pace_fps();
        if (have <= 0 || have > cap + 0.01) {
            cellGcm_set_flip_pace(cap); s_ld_perf_native = cap;
            fprintf(stderr, "[menu] frame generation rests under the panel: the game's limiter set to its pace, %.2f frames/s (was %.2f)\n", cap, have);
        }
    }
}
static void ld_menu_close(const char* how)
{
    if (!s_menu.open) return;
    s_menu.open = 0;
    s_menu.close_us = fsr3_now_us(); s_menu.fade_close = s_menu.fade_now;   /* ovl4: fades out from where it was */
    cellPad_menu_block(0);
    s_menu.esc_guard = 30;   /* frames: an Esc still on its way to the window is not the game's */
    s_menu.close_ms = GetTickCount64();
    ld_menu_save();
    long long nt, in, wr, ps, pin; long bk;
    cellPad_menu_counters(&nt, &in, &wr, &ps, &pin, &bk);
    fprintf(stderr, "[menu] closed by %s at frame %u after %.1f s: the game read the pad %lld times and got a neutral pad every time "
            "(%lld of those reads had buttons or sticks pressed)\n", how, g_ld_frames,
            (double)(s_menu.close_ms - s_menu.open_ms) / 1000.0, nt - s_menu.c_neutral0, in - s_menu.c_input0);
    { long long a, b, c, d, pr; long lp; cellPad_menu_select_counters(&a, &b, &c, &d, &lp, &pr);
      if (pr != s_menu.pause_reads0)
          fprintf(stderr, "[menu] besides those: %lld pad read(s) carried the one SELECT press that paused the game as the panel opened\n",
                  pr - s_menu.pause_reads0);
      fprintf(stderr, "[menu] SELECT so far: %lld hold(s) opened the panel (%lld with a pause press for the game), %lld tap(s) handed on, "
              "%lld pad read(s) with SELECT held back\n", d, c, b, a); }
    if (s_fsr3.on)
        fprintf(stderr, "[menu] frame generation while the panel was open: %llu frames with generation, %llu without\n",
                s_fsr3.tot_gen - s_menu.gen0, s_fsr3.tot_off - s_menu.off0);
    s_menu.c_passed_in0 = pin; s_menu.c_passed0 = ps;
    s_menu.report = 1; s_menu.report_at = 0;
}
/* After closing: when control went back to the game, and 5 s later what the game has read since. */
static void ld_menu_reports(void)
{
    long long nt, in, wr, ps, pin; long bk;
    cellPad_menu_counters(&nt, &in, &wr, &ps, &pin, &bk);
    const unsigned long long now = GetTickCount64();
    if (s_menu.report == 1 && bk >= 0) {
        fprintf(stderr, "[menu] control back to the game %ld ms after closing (every button was up)\n", bk);
        s_menu.report = 2; s_menu.report_at = now + 5000;
    } else if (s_menu.report == 1 && now - s_menu.close_ms > 6000) {
        fprintf(stderr, "[menu] the game has not read the pad since the panel closed (a loading screen?)\n");
        s_menu.report = 0;
    } else if (s_menu.report == 2 && now >= s_menu.report_at) {
        fprintf(stderr, "[menu] 5 s after control came back: the game read the pad %lld times unchanged, %lld of them with buttons or sticks pressed\n",
                ps - s_menu.c_passed0, pin - s_menu.c_passed_in0);
        s_menu.report = 0;
    }
}
static void ld_menu_step(int d)   /* the option above / below in the category shown */
{
    const int c = s_menu.cat, n = k_menu_cat[c].n;
    int i = 0;
    while (i < n && k_menu_cat[c].rows[i] != s_menu.sel) i++;
    i = i >= n ? 0 : (i + d + n) % n;
    s_menu.sel = k_menu_cat[c].rows[i]; s_menu.dirty = 1;
}
static void ld_menu_cat_step(int d)   /* the category before / after (L1 / R1); its first option */
{
    s_menu.cat = (s_menu.cat + d + LD_MENU_NCAT) % LD_MENU_NCAT;
    s_menu.sel = k_menu_cat[s_menu.cat].rows[0]; s_menu.dirty = 1;
}
static void ld_menu_select(int id) { s_menu.sel = id; s_menu.cat = ld_menu_cat_of(id); s_menu.dirty = 1; }

/* LD_MENU_SCRIPT: the events due now. A "shot" waits for its frame dump before the next event runs. */
static void ld_menu_script(void)
{
    while (s_menu.i_ev < s_menu.n_ev) {
        if (s_ld_menu_shot_req > 0) return;
        const double t = (double)(GetTickCount64() - s_menu.t0) / 1000.0;
        if (t < s_menu.ev[s_menu.i_ev].t) return;
        const char* a = s_menu.ev[s_menu.i_ev++].act;
        fprintf(stderr, "[menu] script t=%.1f: %s (frame %u)\n", t, a, g_ld_frames);
        if (!strcmp(a, "open")) ld_menu_open("the test script");
        else if (!strcmp(a, "close")) ld_menu_close("the test script");
        else if (!strcmp(a, "up")) ld_menu_step(-1);
        else if (!strcmp(a, "down")) ld_menu_step(1);
        else if (!strcmp(a, "left")) ld_menu_change(s_menu.sel, -1, -1);
        else if (!strcmp(a, "right") || !strcmp(a, "ok")) ld_menu_change(s_menu.sel, 1, -1);
        else if (!strcmp(a, "shot")) { s_ld_menu_shot_req = 1; return; }
        else if (!strcmp(a, "l1")) ld_menu_cat_step(-1);
        else if (!strcmp(a, "dump")) { s_menu_dump_req = 1; s_menu.dirty = 1; }
        else if (!strcmp(a, "r1")) ld_menu_cat_step(1);
        else if (!strncmp(a, "cat:", 4)) { s_menu.cat = abs(atoi(a + 4)) % LD_MENU_NCAT; s_menu.sel = k_menu_cat[s_menu.cat].rows[0]; s_menu.dirty = 1; }
        else if (!strncmp(a, "lang:", 5)) ld_menu_set_lang(a + 5);
#if defined(NP_TROPHY_TEST) && NP_TROPHY_TEST
        else if (!strncmp(a, "trophy:", 7)) np_trophy_event_push(atoi(a + 7), 0);   /* test builds: a popup only */
#endif
        else if (!strncmp(a, "sel:", 4)) {
            for (int i = 0; i < MR_N; i++) if (!strcmp(s_menu.row[i].key, a + 4)) ld_menu_select(i);
        } else if (!strncmp(a, "set:", 4)) {
            const char* eq = strchr(a + 4, '=');
            int done = 0;
            for (int i = 0; eq && i < MR_N; i++)
                if (strlen(s_menu.row[i].key) == (size_t)(eq - (a + 4)) && !strncmp(s_menu.row[i].key, a + 4, (size_t)(eq - (a + 4)))) {
                    const int to = ld_menu_find(&s_menu.row[i], eq + 1);
                    if (to >= 0) { ld_menu_select(i); ld_menu_change(i, 0, to); done = 1; }
                }
            if (!done) fprintf(stderr, "[menu] script: unknown setting or value in '%s'\n", a);
        }
    }
}

static void ld_menu_init(void)
{
    s_menu.state = -1;
    if (!ld_menu_enabled()) { fprintf(stderr, "[menu] in-game menu off (LD_INGAME_MENU=0)\n"); return; }
    /* ovl4: the copy pass's vertex shader + a pixel shader that scales the premultiplied picture by the fade */
    static const char k_menu_ps[] =
        "Texture2D src : register(t0);\n"
        "SamplerState point_clamp : register(s0);\n"
        "cbuffer PP : register(b0) { float4 pp_uv; float4 pp_opt; };\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 ps(V i) : SV_Target { return src.SampleLevel(point_clamp, i.uv, 0.0) * saturate(pp_opt.z); }\n";
    ID3DBlob* ps = ld_pp_init() && s_pp.copy_vs ? ld_pp_compile(k_menu_ps, sizeof k_menu_ps - 1u, "menu_fade", "ps", "ps_5_0") : NULL;
    s_menu.pso = ps ? ld_pp_pipeline_ex(s_pp.copy_vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, 1) : NULL;
    s_menu.pso_plain = ps ? ld_pp_pipeline_ex(s_pp.copy_vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM, 0) : NULL;
    if (ps) ps->lpVtbl->Release(ps);
    if (!s_menu.pso || !s_menu.pso_plain) { fprintf(stderr, "[menu] no pipeline; no in-game menu\n"); return; }
    /* the key: F1, or LD_MENU_KEY; when the player's key map uses it, the next free F-key */
    int f = 1;
    { const char* e = getenv("LD_MENU_KEY");
      if (e && (e[0] == 'F' || e[0] == 'f') && atoi(e + 1) >= 1 && atoi(e + 1) <= 12) f = atoi(e + 1);
      const char* km = getenv("PS3_KEYMAP");
      for (int tries = 0; km && tries < 10; tries++) {
          char name[8]; snprintf(name, sizeof name, "F%d", f);
          int used = 0;
          for (const char* p = strstr(km, name); p; p = strstr(p + 1, name)) {
              const char before = p == km ? '=' : p[-1], after = p[strlen(name)];
              if ((before == '=' || before == ',') && (after == 0 || after == ',' || after == ';')) used = 1;
          }
          if (!used) break;
          f = f >= 10 ? 1 : f + 1;
      } }
    s_menu.vk = VK_F1 + f - 1; snprintf(s_menu.vk_name, sizeof s_menu.vk_name, "F%d", f);
    /* the game's patches and AMD's files */
    s_menu.p_mlaa = fsr3_patch("Disable SPU MLAA - God of War: Ascension (01)", "Disable SPU MLAA") > 0;
    s_menu.p_unlock = fsr3_patch("Unlock FPS", "Unlock FPS") > 0;
    s_menu.blur_off_start = fsr3_patch("Disable Motion Blur", "Disable Motion Blur") > 0;
    s_menu.can_patch = ascension_patch_set("Disable Motion Blur", -1) == 1;
    s_menu.obj_start = s_fsr3.obj_on;
    { wchar_t dir[MAX_PATH] = L""; wchar_t path[MAX_PATH + 64];
      const char* d = getenv("FSR3_DIR");
      if (d && d[0]) MultiByteToWideChar(CP_UTF8, 0, d, -1, dir, MAX_PATH);
      else { GetModuleFileNameW(NULL, dir, MAX_PATH); wchar_t* s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
      _snwprintf(path, MAX_PATH + 63, L"%ls\\amd_fidelityfx_loader_dx12.dll", dir); path[MAX_PATH + 63] = 0;
      s_menu.fsr_dll = s_fsr3.dll != NULL || GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES; }
    s_menu.fsr_req = -1;
    s_menu.cat = 0; s_menu.sel = k_menu_cat[0].rows[0];   /* ovl4: the first category's first option */
    /* the rows, each at the value the game runs with */
    char v[32];
    ld_menu_row_set(MR_FSR, "AMD FSR 3.1", "fsr", k_menu_fsr, 3, 1, k_menu_fsr[ld_menu_fsr_live()].val, "%s");
    ld_menu_row_set(MR_SMAA, "Edge smoothing (SMAA)", "smaa", k_menu_onoff, 2, 1, ld_smaa_enabled() ? "1" : "0", "%s");
    snprintf(v, sizeof v, "%d", ld_sharpen_strength());
    ld_menu_row_set(MR_SHARP, "Sharpening", "sharpen", k_menu_sharp, 5, 1, v, "%s%%");
    snprintf(v, sizeof v, "%d", ld_vibrance());
    ld_menu_row_set(MR_VIB, "Colour vibrance", "vibrance", k_menu_vib, 5, 1, v, "%s%%");
    ld_menu_row_set(MR_VSYNC, "VSync", "vsync", k_menu_onoff, 2, 1, g_ld_vsync ? "1" : "0", "%s");
    { const char* e = getenv("PS3_FPS_LIMIT");
      /* started with frame generation: FSR replaced the variable with its own pace; the player's limit is the one before */
      const double c = s_fsr3.on && s_fsr3.fg_cap > 0 ? s_fsr3.user_cap : e ? atof(e) : 0.0;
      s_menu.cap_start = c > 0 ? c : 0.0;
      if (c > 0) snprintf(v, sizeof v, "%.4g", c); else snprintf(v, sizeof v, "0");   /* no limit: the "Unlocked" entry */
      ld_menu_row_set(MR_CAP, "Frame rate limit", "fps_cap", k_menu_cap, 4, 1, v, "%s FPS"); }
    { const char* e = getenv("LD_PERF_OVERLAY");
      ld_menu_row_set(MR_PERF, "FPS counter", "perf_overlay", k_menu_onoff, 2, 1, e && e[0] && e[0] != '0' ? "1" : "0", "%s"); }
    { const char* e = getenv("PS3_RENDER_SCALE");
      if (e && e[0]) snprintf(v, sizeof v, "%d", atoi(e)); else snprintf(v, sizeof v, "%u", g_ld_rs * 100u);
      ld_menu_row_set(MR_RS, "Internal resolution", "render_scale", k_menu_rs, 5, 0, v, "%s%%"); }
    { const char* e = getenv("LD_ANISO"); if (!e) e = getenv("PS3_ANISO");
      int a = e ? atoi(e) : 0; if (a < 2) a = 0; else if (a > 16) a = 16;
      snprintf(v, sizeof v, "%d", a);
      ld_menu_row_set(MR_ANISO, "Anisotropic filtering", "aniso", k_menu_aniso, 5, 1, v, "%sx"); }
    ld_menu_row_set(MR_SHADOW, "Dynamic shadows", "no_shadows", k_menu_shadow, 2, 1, cellPad_no_shadows() ? "1" : "0", "%s");
    { const DWORD n = GetEnvironmentVariableW(L"PS3_INGAME_SETTINGS", s_menu.wfile, MAX_PATH - 8);
      if (n == 0 || n >= MAX_PATH - 8) s_menu.wfile[0] = 0;
      /* the C runtime's copy (set after the start by the game's own code), and the name as plain text in the exe:
       * the launcher looks for it there to know that this build has the menu */
      if (!s_menu.wfile[0]) { const char* e = getenv("PS3_INGAME_SETTINGS");
                              if (e && e[0] && !MultiByteToWideChar(CP_ACP, 0, e, -1, s_menu.wfile, MAX_PATH - 8)) s_menu.wfile[0] = 0; }
      if (!s_menu.wfile[0]) s_menu.file[0] = 0;
      else if (!WideCharToMultiByte(CP_UTF8, 0, s_menu.wfile, -1, s_menu.file, (int)sizeof s_menu.file, NULL, NULL)) snprintf(s_menu.file, sizeof s_menu.file, "?"); }
    ld_menu_set_lang(getenv("LD_MENU_LANG"));
    /* LD_MENU_SCRIPT (tests) */
    s_menu.t0 = GetTickCount64();
    { const char* e = getenv("LD_MENU_SCRIPT");
      for (const char* p = e; p && *p && s_menu.n_ev < 96; ) {
          while (*p == ';' || *p == ',' || *p == ' ') p++;
          if (p[0] != 't' || p[1] != '=') break;
          char* end = NULL;
          const double t = strtod(p + 2, &end);
          if (!end || *end != ':') break;
          const char* a = end + 1; const char* q = a + strcspn(a, ";,");   /* ',' too: test runners split their lists at ';' */
          size_t n = (size_t)(q - a); if (n >= sizeof s_menu.ev[0].act) n = sizeof s_menu.ev[0].act - 1;
          s_menu.ev[s_menu.n_ev].t = t; memcpy(s_menu.ev[s_menu.n_ev].act, a, n); s_menu.ev[s_menu.n_ev].act[n] = 0;
          s_menu.n_ev++;
          p = q;
      } }
    s_menu.state = 1;
    g_pad_menu_enabled = 1;
    fprintf(stderr, "[menu] in-game menu ready: %s or hold SELECT opens it; language %s; settings file: %s; patches: Unlock FPS %s, "
            "Disable SPU MLAA %s, Disable Motion Blur %s (%s); FSR files %s; test script: %d event(s)\n",
            s_menu.vk_name, s_menu.lang == 1 ? "es" : s_menu.lang == 2 ? "pt" : "en",
            s_menu.wfile[0] ? s_menu.file : "none (PS3_INGAME_SETTINGS not set: nothing is saved)",
            s_menu.p_unlock ? "on" : "off", s_menu.p_mlaa ? "on" : "off", s_menu.blur_off_start ? "on" : "off",
            s_menu.can_patch ? "can be switched while playing" : "cannot be switched while playing",
            s_menu.fsr_dll ? "found" : "not found", s_menu.n_ev);
}

/* Once per presented frame. draw: 0 = the frame's work only, 1 = that and the panel, 2 = the panel only (bbi's
 * buffer in RENDER_TARGET for 1 and 2). Returns 1 when the panel is to be drawn. */
static int ld_menu_present_panel(u32 bbi, u32 dw, u32 dh, int draw)
{
    if (s_menu.state <= 0) {
        if (s_menu.state < 0) return 0;
        ld_menu_init();
        if (s_menu.state <= 0) return 0;
    }
    if (draw == 2) { const int on = s_menu.open || ld_menu_fading(); if (on) ld_menu_draw(bbi, dw, dh); return on; }
    if (!s_menu.open && !s_menu.busy && s_menu.i_ev >= s_menu.n_ev && !s_menu.report && !s_menu.esc_guard && !s_menu.close_us) {
        /* the whole cost of a menu that is closed: one key, one flag */
        const int down = (GetAsyncKeyState(s_menu.vk) & 0x8000) != 0;
        if (!down) s_menu.key_was = 0;
        if (!down && !g_pad_menu_hold_req) return 0;
    }
    if (s_menu.n_ev) ld_menu_script();
    if (s_menu.report) ld_menu_reports();
    if (s_menu.esc_guard && !--s_menu.esc_guard && !s_menu.open) g_rsx_menu_owns_esc = 0;
    if (!s_menu.open) {
        const int down = (GetAsyncKeyState(s_menu.vk) & 0x8000) != 0;
        const int pressed = down && !s_menu.key_was;
        s_menu.key_was = down;
        if (g_pad_menu_hold_req) { g_pad_menu_hold_req = 0; ld_menu_open("holding SELECT"); }
        else if (pressed && ld_menu_focused()) ld_menu_open(s_menu.vk_name);
        if (!s_menu.open) {   /* ovl4: the fade out after closing */
            if (!ld_menu_fading()) { s_menu.close_us = 0; return 0; }
            if (s_fsr3.on && !ld_fgui_usable()) s_fsr3.menu_hold = 3;
            if (draw == 1) ld_menu_draw(bbi, dw, dh);
            return 1;
        }
    } else {
        g_pad_menu_hold_req = 0;
        const unsigned n = ld_menu_nav();
        const unsigned long long now = GetTickCount64();
        if (s_menu.nav_prev == ~0u) s_menu.nav_prev = n;          /* just opened: what is held does not count */
        const unsigned pressed = n & ~s_menu.nav_prev, released = s_menu.nav_prev & ~n;
        unsigned act = pressed & (MN_UP | MN_DOWN | MN_LEFT | MN_RIGHT | MN_OK | MN_L1 | MN_R1);
        if (act & MN_L1) ld_menu_cat_step(-1);
        else if (act & MN_R1) ld_menu_cat_step(1);
        /* a held direction repeats: after 0.35 s, then every 0.12 s */
        if (act & (MN_UP | MN_DOWN | MN_LEFT | MN_RIGHT)) { s_menu.rep_bit = act & (MN_UP | MN_DOWN | MN_LEFT | MN_RIGHT); s_menu.rep_at = now + 350; }
        else if (s_menu.rep_bit && (n & s_menu.rep_bit) == s_menu.rep_bit) { if (now >= s_menu.rep_at) { act |= s_menu.rep_bit; s_menu.rep_at = now + 120; } }
        else s_menu.rep_bit = 0;
        if (act & MN_UP) ld_menu_step(-1);
        else if (act & MN_DOWN) ld_menu_step(1);
        if (act & MN_LEFT) ld_menu_change(s_menu.sel, -1, -1);
        else if (act & (MN_RIGHT | MN_OK)) ld_menu_change(s_menu.sel, 1, -1);
        /* SELECT held alone again (after it was let go once) closes, like it opened */
        if (!(n & MN_SELECT)) { s_menu.sel_armed = 1; s_menu.sel_since = 0; }
        else if (s_menu.sel_armed) { if (!s_menu.sel_since) s_menu.sel_since = now; }
        if (pressed & MN_BACK) ld_menu_close("Circle");
        else if (released & MN_ESC) ld_menu_close("Esc");          /* on release: the window never sees this Esc as its own */
        else if (pressed & MN_KEY) { s_menu.key_was = 1; ld_menu_close(s_menu.vk_name); }
        else if (s_menu.sel_since && now - s_menu.sel_since >= 500) ld_menu_close("holding SELECT");
        s_menu.nav_prev = n;
        if (!s_menu.open) {   /* just closed: the fade out starts */
            if (s_fsr3.on && !ld_fgui_usable()) s_fsr3.menu_hold = 3;
            if (draw == 1) ld_menu_draw(bbi, dw, dh);
            return 1;
        }
    }
    /* no generated frames under the panel (and for 2 frames after it) -- only when the overlay's layer cannot be used */
    if (s_fsr3.on && !ld_fgui_usable()) s_fsr3.menu_hold = 3;
    if (draw == 1) ld_menu_draw(bbi, dw, dh);
    return 1;
}
/* host/trophies: the panel (above) and the trophy popup. Same contract as before: draw 0 = the frame's work, 1 = that
 * and the drawing, 2 = the drawing only; returns 1 when something is to be drawn. */
static int ld_menu_present(u32 bbi, u32 dw, u32 dh, int draw)
{
    const int toast = draw == 2 ? (s_toast.st > 0 && s_toast.cur >= 0) : ld_toast_tick();
    const int panel = ld_menu_present_panel(bbi, dw, dh, draw);
    if (toast && draw != 0) ld_toast_draw(bbi, dw, dh);
    return panel || toast;
}
#endif
