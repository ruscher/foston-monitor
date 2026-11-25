"""A janela do BigFoston OS Installer: central, fluxos de instalar, atualizar, verificar e restaurar.

Tudo que demora (abrir/conferir pacote, listar cartões, gravar, backup) roda fora da thread da
interface (common.run_bg); o progresso mostrado são os bytes de verdade.

SPDX-License-Identifier: GPL-3.0-or-later
"""
from __future__ import annotations

import os
import time

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Adw, Gio, GLib, Gtk  # noqa: E402

from .. import __version__
from ..core import backup as bk
from ..core import install as inst
from ..core.manifest import load_devices
from ..core.package import open_package
from ..core.storage import Storage, blockers, warnings
from .common import STATE_TEXT, Page, _, human_size, label, picture, run_bg, status_icon

GITHUB = "https://github.com/ruscher/foston-monitor"
ISSUES = GITHUB + "/issues/new"
DOCS = GITHUB + "/blob/main/docs/dual-boot.md"

TERMS = _(
    "O BigFoston OS é um projeto independente e de código aberto, desenvolvido pela comunidade, sem vínculo "
    "oficial com o fabricante FOSTON.\n\n"
    "A instalação e utilização deste software são realizadas por conta e risco do usuário.\n\n"
    "O instalador foi projetado para preservar o sistema operacional original e permitir o retorno ao Windows "
    "CE. Entretanto, diferenças entre modelos, revisões de hardware, falhas de armazenamento, interrupções de "
    "energia ou procedimentos incorretos podem provocar perda de dados, problemas de funcionamento ou, em "
    "determinadas circunstâncias, tornar o equipamento inutilizável.\n\n"
    "É indispensável realizar backup dos arquivos importantes antes de iniciar.\n\n"
    "Não desligue o aparelho, não remova o cartão SD e não desconecte o cabo USB durante operações de gravação.\n\n"
    "Utilize exclusivamente modelos e revisões de hardware compatíveis com a versão selecionada.\n\n"
    "O projeto é disponibilizado conforme sua licença de software livre (GPL-3.0) e sem garantia, observados os "
    "direitos previstos na legislação aplicável.")
CONFIRMS = (
    _("Li e compreendi os termos de utilização."),
    _("Compreendo que a instalação é realizada por minha conta e risco."),
    _("Compreendo a importância de realizar backup dos meus arquivos."),
    _("Estou ciente de que devo verificar a compatibilidade do meu equipamento."),
)

# guia do aparelho depois de preparar o cartão (texto conferido com o aparelho real e a doc)
GUIDE_INSTALL = (
    (_("Retire o cartão"), _("O cartão já foi ejetado com segurança. Tire-o do leitor."), None),
    (_("Desligue o FOSTON"), _("Use o botão de energia. Não é preciso o reset (furinho) nem restaurar "
                                "configurações de fábrica."), None),
    (_("Coloque o cartão no FOSTON"), _("No encaixe do cartão SD, com o aparelho desligado."), None),
    (_("Ligue o FOSTON"), _("O sistema original (Windows CE) aparece normalmente."), None),
    (_("Anote o caminho do GPS"), _("Em Ajustes → Caminho da navegação, anote (ou fotografe) o caminho atual do "
                                    "navegador. Ele será guardado também pelo agente, mas a anotação ajuda a "
                                    "voltar se algo sair errado."), None),
    (_("Aponte a navegação para o agente"), _("Ainda em Caminho da navegação, escolha o arquivo no cartão: "
                                              "pasta BigFoston → bfagent.exe. Confirme e volte ao menu."), None),
    (_("Toque em Navegação"), _("O agente abre a tela Dual Boot. Toque em “Instalar o Dual Boot” e confirme: "
                                "ele grava o BigFoston OS na memória interna e mostra o resultado."), "dual-boot.png"),
    (_("Desligue e ligue"), _("Depois de instalado, ao ligar aparece a tela Dual Boot com a contagem: o "
                              "BigFoston OS inicia sozinho, ou toque em Windows CE para ficar no sistema "
                              "original."), "foto-bigfoston.jpg"),
)
GUIDE_UPDATE = (
    (_("Retire o cartão e coloque no FOSTON"), _("Com o aparelho desligado."), None),
    (_("Ligue o FOSTON"), _("A tela Dual Boot mostra “Preparando o início do BigFoston...” enquanto grava a "
                            "versão nova na memória interna. Não desligue nesse momento."), "dual-boot.png"),
    (_("Confira a versão"), _("Depois que o BigFoston OS iniciar, veja em Ajustes → Sistema. O resultado da "
                              "atualização também fica no cartão, em BigFoston/update.log (APLICADA ou "
                              "RECUSADA)."), None),
)


class State:
    def __init__(self):
        self.mode = "install"          # install, update, repair
        self.package = None
        self.card = None
        self.backup_scope = bk.SCOPE_BIGFOSTON
        self.backup_dest = os.path.join(GLib.get_home_dir(), "BigFoston-Backups")
        self.do_backup = True
        self.backup_done = ""
        self.log: list[str] = []


class InstallerWindow(Adw.ApplicationWindow):
    def __init__(self, app, storage: Storage | None, storage_error: str = ""):
        super().__init__(application=app, title=_("BigFoston OS Installer"))
        self.set_default_size(900, 720)
        self.set_size_request(360, 480)
        self.storage, self.storage_error = storage, storage_error
        self.state = State()
        self.toasts = Adw.ToastOverlay()
        self.nav = Adw.NavigationView()
        self.toasts.set_child(self.nav)
        self.set_content(self.toasts)
        self.nav.add(self.home_page())

    # ------------------------------------------------------------ utilidades

    def toast(self, text: str):
        self.toasts.add_toast(Adw.Toast(title=text, timeout=4))

    def error(self, heading: str, err):
        msg = str(err) or err.__class__.__name__
        d = Adw.AlertDialog(heading=heading, body=msg)
        d.add_response("ok", _("Fechar"))
        d.present(self)

    def open_uri(self, uri: str):
        Gtk.UriLauncher(uri=uri).launch(self, None, None, None)

    def push(self, page):
        self.nav.push(page)

    # ------------------------------------------------------------ central

    def home_page(self):
        page = Page(_("BigFoston OS Installer"), "home", width=880)
        menu = Gio.Menu()
        menu.append(_("Documentação no GitHub"), "win.docs")
        menu.append(_("Reportar um problema"), "win.issue")
        menu.append(_("Sobre o BigFoston OS Installer"), "win.about")
        page.header.pack_end(Gtk.MenuButton(icon_name="open-menu-symbolic", menu_model=menu,
                                            tooltip_text=_("Menu principal")))
        for name, cb in (("docs", lambda *_: self.open_uri(DOCS)), ("issue", lambda *_: self.open_uri(ISSUES)),
                         ("about", lambda *_: self.about())):
            act = Gio.SimpleAction.new(name, None)
            act.connect("activate", cb)
            self.add_action(act)

        hero = Gtk.Box(spacing=24, halign=Gtk.Align.CENTER)
        hero.append(picture("fostonos-mark.png", 110))
        txt = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, valign=Gtk.Align.CENTER)
        txt.append(label(_("Bem-vindo ao BigFoston OS"), ("hero-title",)))
        txt.append(label(_("Transforme seu navegador GPS FOSTON num dispositivo Linux moderno e dê uma nova "
                           "vida ao seu equipamento, sem apagar o sistema original."), ("dim-label",)))
        hero.append(txt)
        page.add(hero)
        if not self.storage:
            ban = Adw.Banner(title=_("Os cartões não podem ser lidos: ") + self.storage_error, revealed=True)
            page.add(ban)

        flow = Gtk.FlowBox(selection_mode=Gtk.SelectionMode.NONE, column_spacing=12, row_spacing=12,
                           max_children_per_line=3, min_children_per_line=1, homogeneous=True)
        cards = (
            ("system-software-install-symbolic", _("Instalar BigFoston OS"),
             _("Prepare o cartão SD e instale o sistema, mantendo o Windows CE."), self.start_install),
            ("software-update-available-symbolic", _("Atualizar BigFoston OS"),
             _("Leve uma versão nova ao aparelho, preservando seus dados."), self.start_update),
            ("emblem-system-symbolic", _("Verificar e reparar"),
             _("Confira os arquivos do cartão e corrija o que estiver danificado."), self.start_repair),
            ("edit-undo-symbolic", _("Restaurar o FOSTON original"),
             _("Volte ao funcionamento original, temporária ou definitivamente."), self.start_restore),
            ("computer-symbolic", _("Compatibilidade"),
             _("Veja os modelos suportados e como conferir o seu aparelho."), lambda: self.push(self.compat_page(False))),
            ("help-browser-symbolic", _("Guia do aparelho"),
             _("Os passos no FOSTON, para consultar a qualquer momento."), lambda: self.push(self.guide_page(GUIDE_INSTALL, final=False))),
        )
        for icon, title, desc, cb in cards:
            b = Gtk.Button()
            b.add_css_class("card")
            b.add_css_class("choice-card")
            v = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
            im = Gtk.Image(icon_name=icon, pixel_size=32, halign=Gtk.Align.START)
            v.append(im)
            t = label(title, ("heading",))
            t.set_max_width_chars(24)
            v.append(t)
            d = label(desc, ("dim-label", "caption"))
            d.set_max_width_chars(30)
            v.append(d)
            b.set_child(v)
            b.connect("clicked", lambda _b, f=cb: f())
            flow.append(b)
        page.add(flow)
        return page

    def about(self):
        d = Adw.AboutDialog(application_name=_("BigFoston OS Installer"), version=__version__,
                            developer_name="BigLinux / BigFoston", license_type=Gtk.License.GPL_3_0,
                            website=GITHUB, issue_url=ISSUES,
                            comments=_("Instalador, atualizador e ferramenta de recuperação do BigFoston OS."))
        d.present(self)

    # ------------------------------------------------------------ fluxos

    def start_install(self):
        self.state = State()
        self.state.mode = "install"
        self.push(self.terms_page())

    def start_update(self):
        self.state = State()
        self.state.mode = "update"
        self.push(self.version_page())

    def start_repair(self):
        self.state = State()
        self.state.mode = "repair"
        self.push(self.card_page())

    def start_restore(self):
        self.push(self.restore_page())

    # ------------------------------------------------------------ termos

    def terms_page(self):
        page = Page(_("Termos de utilização"), "terms", _("Aceitar e continuar"),
                    lambda: self.push(self.compat_page(True)))
        page.next_button.set_sensitive(False)
        page.add(label(_("Aviso importante antes de continuar"), ("title-2",)))
        page.add(label(TERMS))
        group = Adw.PreferencesGroup()
        checks = []

        def changed(*_a):
            page.next_button.set_sensitive(all(c.get_active() for c in checks))
        for text in CONFIRMS:
            c = Gtk.CheckButton()
            row = Adw.ActionRow(title=text, activatable_widget=c)
            row.set_title_lines(0)
            row.add_prefix(c)
            c.connect("toggled", changed)
            checks.append(c)
            group.add(row)
        page.add(group)
        box = Gtk.Box(spacing=12)
        lic = Gtk.Button(label=_("Ler licença completa"))
        lic.connect("clicked", lambda *_: self.open_uri(GITHUB + "/blob/main/LICENSE"))
        save = Gtk.Button(label=_("Salvar os termos"))
        save.connect("clicked", lambda *_: self.save_terms())
        box.append(lic)
        box.append(save)
        page.add(box)
        page.add(label(_("Os termos não substituem as proteções do instalador: ele nunca grava o firmware, o "
                         "bootloader ou o Windows CE, e só altera o que você confirmar."), ("dim-label", "caption")))
        return page

    def save_terms(self):
        dlg = Gtk.FileDialog(initial_name="BigFoston-OS-termos.txt")

        def done(d, res):
            try:
                f = d.save_finish(res)
            except GLib.Error:
                return
            text = TERMS + "\n\n" + "\n".join("- " + c for c in CONFIRMS) + "\n"
            f.replace_contents(text.encode(), None, False, Gio.FileCreateFlags.REPLACE_DESTINATION, None)
            self.toast(_("Termos salvos"))
        dlg.save(self, None, done)

    # ------------------------------------------------------------ compatibilidade

    def compat_page(self, in_flow: bool):
        page = Page(_("Compatibilidade"), "compat", _("Meu aparelho é compatível") if in_flow else None,
                    (lambda: self.push(self.version_page())) if in_flow else None)
        for dev in load_devices():
            row = Gtk.Box(spacing=18)
            pic = picture("foston-fs707dc.png", 150, can_shrink=False)
            pic.set_size_request(150, 150)
            row.append(pic)
            col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, valign=Gtk.Align.CENTER, hexpand=True)
            col.append(label(dev.name, ("title-2",)))
            st = Gtk.Box(spacing=6)
            st.append(Gtk.Image(icon_name=status_icon(dev.state)))
            st.append(label(STATE_TEXT[dev.state], ("heading",)))
            col.append(st)
            col.append(label(dev.notes, ("dim-label",)))
            row.append(col)
            page.add(row)
            g = Adw.PreferencesGroup(title=_("Unidade de referência"))
            for t, v in ((_("Processador"), dev.cpu), (_("Sistema original"), dev.os), (_("Tela"), dev.screen),
                         (_("Cartão SD"), _("FAT32; testados no aparelho: 2 GB (SD) e 32 GB (SDHC)"))):
                r = Adw.ActionRow(title=t, subtitle=v)
                r.set_subtitle_lines(0)
                g.add(r)
            page.add(g)
        how = Adw.PreferencesGroup(title=_("Como conferir o seu"),
                                   description=_("No FOSTON: Ajustes → seta para a direita → Sistema de Informação. "
                                                 "O instalador só aceita o modelo da lista; outros modelos com "
                                                 "Windows CE não são compatíveis só por isso."))
        page.add(how)
        need = Adw.PreferencesGroup(title=_("O que você precisa"))
        for t, s in ((_("Um leitor de cartões SD no computador"), _("Recomendado: é mais rápido e confiável que o "
                                                                     "cabo USB do FOSTON para arquivos grandes.")),
                     (_("Um cartão SD em boas condições"), _("FAT32, até 32 GB. Cartões com defeito de gravação "
                                                             "são recusados na conferência.")),
                     (_("Bateria carregada ou fonte"), _("Não desligue o aparelho enquanto ele grava a memória "
                                                         "interna.")),
                     (_("Uns 15 minutos"), _("Sem usar o GPS para navegar durante a instalação."))):
            r = Adw.ActionRow(title=t, subtitle=s)
            r.set_subtitle_lines(0)
            r.add_prefix(Gtk.Image(icon_name="object-select-symbolic"))
            need.add(r)
        page.add(need)
        return page

    # ------------------------------------------------------------ versão

    def version_page(self):
        page = Page(_("Versão do BigFoston OS"), "version", _("Continuar"), self.after_version)
        page.next_button.set_sensitive(self.state.package is not None)
        page.add(label(_("Escolha o pacote da versão (o arquivo .zip baixado do GitHub Releases, ou a pasta "
                         "dele). O instalador confere cada arquivo antes de usar."), ("dim-label",)))
        group = Adw.PreferencesGroup()
        row = Adw.ActionRow(title=_("Arquivo da versão"), subtitle=_("Nenhum escolhido"), activatable=True)
        row.add_prefix(Gtk.Image(icon_name="package-x-generic-symbolic"))
        pick = Gtk.Button(label=_("Escolher..."), valign=Gtk.Align.CENTER)
        row.add_suffix(pick)
        group.add(row)
        page.add(group)
        info = Adw.PreferencesGroup(title=_("Detalhes"))
        info.set_visible(False)
        page.add(info)
        spinner = Gtk.Spinner(visible=False)
        page.add(spinner)
        rows = []

        def show(pkg):
            spinner.stop()
            spinner.set_visible(False)
            self.state.package = pkg
            r = pkg.release
            row.set_subtitle(os.path.basename(pkg.source))
            for x in rows:
                info.remove(x)
            rows.clear()
            dev = next((d for d in load_devices() if d.id == r.device), None)
            for t, v in ((_("Versão"), f"BigFoston OS {r.version}"), (_("Data"), r.date or "—"),
                         (_("Canal"), {"stable": _("Estável"), "beta": "Beta", "experimental": _("Experimental")}[r.channel]),
                         (_("Aparelho"), dev.name if dev else r.device), (_("Agente do Windows CE"), r.agent_version),
                         (_("Tamanho"), human_size(r.total_size)), (_("Integridade"), _("SHA-256 de todos os arquivos conferido"))):
                x = Adw.ActionRow(title=t, subtitle=v)
                info.add(x)
                rows.append(x)
            if r.notes:
                x = Adw.ActionRow(title=_("Notas"), subtitle=r.notes)
                x.set_subtitle_lines(0)
                info.add(x)
                rows.append(x)
            info.set_visible(True)
            ok = dev is not None and dev.installable
            page.next_button.set_sensitive(ok)
            if not ok:
                self.error(_("Pacote de outro aparelho"), _("Este pacote é para “%s”, que não está na lista de "
                                                           "aparelhos compatíveis.") % r.device)

        def fail(e):
            spinner.stop()
            spinner.set_visible(False)
            self.state.package = None
            page.next_button.set_sensitive(False)
            self.error(_("Pacote inválido"), e)

        def chosen(path):
            spinner.set_visible(True)
            spinner.start()

            def work():
                pkg = open_package(path)
                pkg.verify()
                return pkg
            run_bg(work, show, fail)

        def on_pick(*_a):
            dlg = Gtk.FileDialog(title=_("Pacote do BigFoston OS"))
            filt = Gtk.FileFilter(name=_("Pacote do BigFoston OS (.zip)"))
            filt.add_pattern("*.zip")
            store = Gio.ListStore.new(Gtk.FileFilter)
            store.append(filt)
            dlg.set_filters(store)

            def done(d, res):
                try:
                    f = d.open_finish(res)
                except GLib.Error:
                    return
                chosen(f.get_path())
            dlg.open(self, None, done)
        pick.connect("clicked", on_pick)
        row.connect("activated", on_pick)
        folder = Gtk.Button(label=_("Escolher uma pasta em vez do .zip"), halign=Gtk.Align.START)
        folder.add_css_class("flat")

        def on_folder(*_a):
            dlg = Gtk.FileDialog(title=_("Pasta da versão"))

            def done(d, res):
                try:
                    f = d.select_folder_finish(res)
                except GLib.Error:
                    return
                chosen(f.get_path())
            dlg.select_folder(self, None, done)
        folder.connect("clicked", on_folder)
        page.add(folder)
        if self.state.package:
            show(self.state.package)
        return page

    def after_version(self):
        if self.state.mode == "install":
            self.push(self.method_page())
        else:
            self.push(self.card_page())

    # ------------------------------------------------------------ método

    def method_page(self):
        page = Page(_("Método de instalação"), "method", _("Continuar"), lambda: self.push(self.card_page()))
        group = Adw.PreferencesGroup(description=_("O sistema é gravado no cartão SD; depois, o próprio FOSTON "
                                                   "copia para a memória interna, sem cabo."))
        first = None
        opts = (
            ("media-flash-symbolic", _("Cartão SD no leitor do computador"), _("Recomendado. Com o FOSTON desligado, "
             "tire o cartão e coloque-o no leitor de cartões do computador. Mais rápido, confiável e verificável."), True),
            ("drive-removable-media-symbolic", _("Cabo USB (MASS STORAGE)"), _("No FOSTON: Ajustes → seta para a "
             "direita → USB → MASS STORAGE, e ligue o cabo de dados. Funciona para conferir e para o agente, mas "
             "a cópia de arquivos grandes por esse caminho já falhou no aparelho: prefira o leitor."), True),
            ("network-wired-symbolic", _("MS ACTIVESYNC"), _("Só informativo: este instalador não usa o ActiveSync "
             "(não há forma testada de gravar e conferir arquivos por ele no Linux)."), False),
        )
        for icon, title, desc, enabled in opts:
            c = Gtk.CheckButton()
            if first:
                c.set_group(first)
            else:
                first = c
                c.set_active(True)
            row = Adw.ActionRow(title=title, subtitle=desc, activatable_widget=c)
            row.set_subtitle_lines(0)
            row.add_prefix(c)
            row.add_suffix(Gtk.Image(icon_name=icon, pixel_size=24))
            row.set_sensitive(enabled)
            group.add(row)
        page.add(group)
        return page

    # ------------------------------------------------------------ cartão

    def card_page(self):
        repair = self.state.mode == "repair"
        page = Page(_("Escolha o cartão SD"), "card", _("Verificar este cartão") if repair else _("Continuar"),
                    self.after_card)
        page.next_button.set_sensitive(False)
        page.add(label(_("Só aparecem como opção os cartões que podem ser usados com segurança. Os outros discos "
                         "ficam listados embaixo, com o motivo."), ("dim-label",)))
        good = Adw.PreferencesGroup(title=_("Cartões"))
        bad = Adw.PreferencesGroup(title=_("Outros discos (não podem ser usados)"))
        page.add(good)
        page.add(bad)
        refresh = Gtk.Button(icon_name="view-refresh-symbolic", tooltip_text=_("Procurar de novo"))
        page.header.pack_end(refresh)
        assist = Gtk.Button(label=_("Não sabe qual é o seu cartão?"), halign=Gtk.Align.START)
        assist.add_css_class("flat")
        page.add(assist)
        rows = []
        self.state.card = None

        def select(card, check):
            if check.get_active():
                self.state.card = card
                page.next_button.set_sensitive(True)

        def fill(cards):
            for g, r in rows:
                g.remove(r)
            rows.clear()
            group_check = None
            prev = self.state.card
            self.state.card = None
            page.next_button.set_sensitive(False)
            n_good = 0
            for c in cards:
                why = blockers(c)
                title = f"{c.title} — {human_size(c.size)}"
                sub = [c.device, c.label or _("sem rótulo"), c.fs_type.upper() or "?"]
                if c.has_bigfoston:
                    sub.append(_("já tem BigFoston"))
                if why:
                    r = Adw.ActionRow(title=title, subtitle=" · ".join(sub) + "\n" + "; ".join(why))
                    r.set_subtitle_lines(0)
                    r.add_prefix(Gtk.Image(icon_name="action-unavailable-symbolic"))
                    bad.add(r)
                    rows.append((bad, r))
                    continue
                n_good += 1
                chk = Gtk.CheckButton()
                if group_check:
                    chk.set_group(group_check)
                else:
                    group_check = chk
                r = Adw.ActionRow(title=title, subtitle=" · ".join(sub), activatable_widget=chk)
                r.set_subtitle_lines(0)
                r.add_prefix(chk)
                if c.free >= 0:
                    bar = Gtk.LevelBar(min_value=0, max_value=1, value=max(0.0, 1 - c.free / max(c.size, 1)),
                                       valign=Gtk.Align.CENTER, width_request=110)
                    bar.add_css_class("usage-bar")
                    bar.set_tooltip_text(_("%s livres") % human_size(c.free))
                    r.add_suffix(bar)
                for w in warnings(c):
                    r.set_subtitle(r.get_subtitle() + "\n⚠ " + w)
                chk.connect("toggled", lambda ch, cc=c: select(cc, ch))
                good.add(r)
                rows.append((good, r))
                if prev and prev.key == c.key:
                    chk.set_active(True)
            if not n_good:
                r = Adw.ActionRow(title=_("Nenhum cartão encontrado"),
                                  subtitle=_("Coloque o cartão SD no leitor. Ele aparece aqui sozinho."))
                r.add_prefix(Gtk.Image(icon_name="media-flash-symbolic"))
                good.add(r)
                rows.append((good, r))

        def reload(*_a):
            if self.storage:
                run_bg(self.storage.list_cards, fill, lambda e: self.error(_("Erro ao ler os discos"), e))
        refresh.connect("clicked", reload)
        assist.connect("clicked", lambda *_: self.assist_dialog(fill))
        if self.storage:
            sub = self.storage.watch(lambda: GLib.timeout_add(600, lambda: (reload(), False)[1]))
            page.connect("unrealize", lambda *_: self.storage.unwatch(sub))
        reload()
        return page

    def assist_dialog(self, fill):
        """Tirar e pôr o cartão: o que aparece de novo é ele (ainda pedindo confirmação)."""
        if not self.storage:
            return
        d = Adw.AlertDialog(heading=_("Vamos encontrar o seu cartão"),
                            body=_("1. Retire o cartão SD do leitor agora.\n2. Espere esta mensagem mudar.\n"
                                   "3. Coloque o cartão de novo."))
        d.add_response("cancel", _("Cancelar"))
        before = {"keys": None, "phase": 0}

        def snapshot(cards):
            before["keys"] = {c.key for c in cards}

        def changed():
            def got(cards):
                keys = {c.key for c in cards}
                if before["phase"] == 0 and before["keys"] is not None and keys < before["keys"]:
                    before["phase"] = 1
                    before["keys"] = keys
                    d.set_body(_("Cartão retirado. Agora coloque-o de novo no leitor."))
                elif before["phase"] == 1 and keys - before["keys"]:
                    new = [c for c in cards if c.key in keys - before["keys"]]
                    fill(cards)
                    d.close()
                    c = new[0]
                    self.toast(_("Seu cartão: %s (%s)") % (c.title, c.device) + " — " + _("marque-o na lista"))
            run_bg(self.storage.list_cards, got)
        run_bg(self.storage.list_cards, snapshot)
        sub = self.storage.watch(lambda: GLib.timeout_add(800, lambda: (changed(), False)[1]))
        d.connect("closed", lambda *_: self.storage.unwatch(sub))
        d.present(self)

    def after_card(self):
        c = self.state.card
        if not c:
            return
        if self.state.mode == "repair":
            self.push(self.repair_page())
        else:
            self.push(self.backup_page())

    # ------------------------------------------------------------ backup

    def backup_page(self):
        page = Page(_("Backup"), "backup", _("Continuar"), lambda: self.push(self.summary_page()))
        page.add(label(_("O instalador não apaga nada do cartão: só grava na pasta BigFoston. Mesmo assim, "
                         "guarde uma cópia antes de continuar."), ("dim-label",)))
        g = Adw.PreferencesGroup()
        sw = Adw.SwitchRow(title=_("Fazer backup antes de gravar"), active=True)
        g.add(sw)
        scope = Adw.ComboRow(title=_("O que copiar"), model=Gtk.StringList.new(
            [_("A pasta BigFoston (rápido)"), _("O cartão inteiro (mapas e tudo)")]))
        g.add(scope)
        dest = Adw.ActionRow(title=_("Destino"), subtitle=self.state.backup_dest, activatable=True)
        dest.add_suffix(Gtk.Image(icon_name="folder-open-symbolic"))
        g.add(dest)
        page.add(g)

        def on_dest(*_a):
            dlg = Gtk.FileDialog(title=_("Pasta do backup"))

            def done(d, res):
                try:
                    f = d.select_folder_finish(res)
                except GLib.Error:
                    return
                self.state.backup_dest = f.get_path()
                dest.set_subtitle(f.get_path())
            dlg.select_folder(self, None, done)
        dest.connect("activated", on_dest)

        def sync(*_a):
            self.state.do_backup = sw.get_active()
            self.state.backup_scope = bk.SCOPE_ALL if scope.get_selected() == 1 else bk.SCOPE_BIGFOSTON
            scope.set_sensitive(sw.get_active())
            dest.set_sensitive(sw.get_active())
        sw.connect("notify::active", sync)
        scope.connect("notify::selected", sync)
        sync()
        return page

    # ------------------------------------------------------------ resumo

    def summary_page(self):
        s = self.state
        r = s.package.release
        page = Page(_("Resumo"), "summary", _("Preparar o cartão"), self.confirm_run)
        g = Adw.PreferencesGroup(title=_("O que será feito"))
        for t, v in ((_("Versão"), f"BigFoston OS {r.version} — {_('agente')} {r.agent_version}"),
                     (_("Cartão"), f"{s.card.title} — {s.card.device} ({human_size(s.card.size)})"),
                     (_("Espaço usado"), human_size(r.total_size)),
                     (_("Backup"), (_("sim") + " → " + s.backup_dest) if s.do_backup else _("não"))):
            x = Adw.ActionRow(title=t, subtitle=v)
            x.set_subtitle_lines(0)
            g.add(x)
        page.add(g)
        will = Adw.PreferencesGroup(title=_("No cartão"))
        for t in (_("Grava a pasta BigFoston/update com o sistema e confere cada arquivo"),
                  _("Grava o agente BigFoston/bfagent.exe (o anterior fica como bfagent.exe.old)")):
            x = Adw.ActionRow(title=t)
            x.set_title_lines(0)
            x.add_prefix(Gtk.Image(icon_name="object-select-symbolic"))
            will.add(x)
        page.add(will)
        wont = Adw.PreferencesGroup(title=_("Não será feito"))
        for t in (_("Formatar o cartão ou apagar seus arquivos (mapas, iGO, músicas)"),
                  _("Gravar o firmware, o bootloader ou o Windows CE"),
                  _("Mudar o aparelho agora: a memória interna só é gravada quando você tocar em “Instalar o "
                    "Dual Boot” no FOSTON, e o caminho da navegação só muda quando você mesmo o trocar")):
            x = Adw.ActionRow(title=t)
            x.set_title_lines(0)
            x.add_prefix(Gtk.Image(icon_name="window-close-symbolic"))
            wont.add(x)
        page.add(wont)
        return page

    def confirm_run(self):
        c = self.state.card
        d = Adw.AlertDialog(heading=_("Preparar o cartão?"),
                            body=_("Vai gravar a pasta BigFoston em %s (%s). Não retire o cartão até o fim.")
                            % (c.title, c.device))
        d.add_response("cancel", _("Cancelar"))
        d.add_response("go", _("Preparar"))
        d.set_response_appearance("go", Adw.ResponseAppearance.SUGGESTED)
        d.connect("response", lambda _d, rid: rid == "go" and self.push(self.progress_page()))
        d.present(self)

    # ------------------------------------------------------------ progresso

    def progress_page(self):
        s = self.state
        page = Page(_("Preparando o cartão"), "progress", _("Cancelar"))
        page.set_can_pop(False)
        page.next_button.remove_css_class("suggested-action")
        stage = page.add(label(_("Começando..."), ("title-3",)))
        bar = page.add(Gtk.ProgressBar(show_text=True))
        detail = page.add(label("", ("dim-label", "numeric")))
        exp = Gtk.Expander(label=_("Registro técnico"))
        buf = Gtk.TextBuffer()
        tv = Gtk.TextView(buffer=buf, editable=False, monospace=True, wrap_mode=Gtk.WrapMode.WORD_CHAR)
        tv.add_css_class("mono-log")
        sc = Gtk.ScrolledWindow(min_content_height=160)
        sc.set_child(tv)
        exp.set_child(sc)
        page.add(exp)
        t0 = {"t": time.monotonic(), "stage": None, "last": 0.0}
        job = {"obj": None}

        def log(text):
            line = time.strftime("%H:%M:%S ") + text
            s.log.append(line)
            buf.insert(buf.get_end_iter(), line + "\n")

        def on_step(step):
            now = time.monotonic()
            if step.stage != t0["stage"]:
                t0.update(t=now, stage=step.stage)
                log(step.message)
            elif now - t0["last"] < 0.1 and step.done != step.total:
                return False
            t0["last"] = now
            stage.set_text(step.message)
            if step.total:
                frac = step.done / step.total
                bar.set_fraction(frac)
                el = now - t0["t"]
                speed = step.done / el if el > 0.2 else 0
                eta = (step.total - step.done) / speed if speed > 0 else 0
                txt = f"{human_size(step.done)} / {human_size(step.total)}"
                if speed:
                    txt += f" · {human_size(int(speed))}/s"
                if eta > 1:
                    txt += " · " + _("faltam %d s") % int(eta)
                detail.set_text(txt)
            else:
                bar.pulse()
            if step.stage == "commit":
                page.next_button.set_sensitive(False)
                page.next_button.set_tooltip_text(_("Trocando os nomes dos arquivos: não dá para parar agora."))
            return False

        def report(step):
            GLib.idle_add(on_step, step)

        def cancel(*_a):
            if job["obj"] and not job["obj"].cancel():
                self.toast(_("Não dá para cancelar agora: o cartão está sendo finalizado."))
            else:
                log(_("Cancelamento pedido"))
        page.next_button.connect("clicked", cancel)

        def work():
            if s.do_backup:
                mount = self.storage.mount(s.card)
                os.makedirs(s.backup_dest, exist_ok=True)
                b = bk.Backup(s.card, mount, s.backup_dest, s.backup_scope, report)
                job["obj"] = b
                s.backup_done = b.run()
                GLib.idle_add(log, _("Backup em ") + s.backup_done)
            ins = inst.Installer(self.storage, s.card, s.package, report)
            job["obj"] = ins
            info = ins.run()
            self.storage.eject(ins.card)
            return info

        def done(info):
            log(_("Cartão preparado, conferido e ejetado"))
            self.push(self.guide_page(GUIDE_INSTALL if s.mode == "install" else GUIDE_UPDATE, final=True))

        def fail(e):
            page.set_can_pop(True)
            page.next_button.set_label(_("Voltar"))
            page.next_button.set_sensitive(True)
            page.next_button.connect("clicked", lambda *_: self.nav.pop())
            if isinstance(e, inst.Cancelled):
                stage.set_text(_("Cancelado: o cartão ficou como estava."))
                log(_("Cancelado"))
                return
            stage.set_text(_("Não foi possível preparar o cartão"))
            log(f"ERRO: {e}")
            self.error(_("Não foi possível preparar o cartão"), e)
        run_bg(work, done, fail)
        return page

    # ------------------------------------------------------------ guia do aparelho

    def guide_page(self, steps, final: bool):
        page = Page(_("No FOSTON") if final else _("Guia do aparelho"), "guide",
                    _("Concluir") if final else None, (lambda: self.push(self.done_page())) if final else None)
        if final:
            page.set_can_pop(False)
            page.add(Adw.Banner(title=_("Cartão preparado e conferido. Agora, no aparelho:"), revealed=True))
        for i, (title, text, img) in enumerate(steps, 1):
            row = Gtk.Box(spacing=14)
            n = Gtk.Label(label=str(i), valign=Gtk.Align.START)
            n.add_css_class("step-number")
            row.append(n)
            col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, hexpand=True)
            col.append(label(title, ("heading",)))
            col.append(label(text))
            if img:
                p = picture(img, 220)
                p.add_css_class("device-photo")
                col.append(p)
            row.append(col)
            page.add(row)
        page.add(label(_("Para voltar ao sistema original a qualquer momento: na tela Dual Boot, toque em "
                         "Windows CE. Para desfazer tudo, use “Restaurar o FOSTON original” na tela inicial "
                         "deste instalador."), ("dim-label",)))
        return page

    def done_page(self):
        s = self.state
        page = Page(_("Concluído"), "done", _("Concluir"), lambda: self.nav.pop_to_tag("home"))
        page.set_can_pop(False)
        sp = Adw.StatusPage(title=_("Cartão pronto!"),
                            description=_("O BigFoston OS %s está no cartão e foi conferido. A instalação termina "
                                          "no aparelho, quando você tocar em “Instalar o Dual Boot” (ou, numa "
                                          "atualização, ao ligar).") % s.package.release.version)
        sp.set_paintable(Gtk.Picture.new_for_filename(
            os.path.join(os.path.dirname(__file__), "..", "data", "images", "fostonos-mark.png")).get_paintable())
        page.add(sp)
        g = Adw.PreferencesGroup()
        if s.backup_done:
            r = Adw.ActionRow(title=_("Backup"), subtitle=s.backup_done, activatable=True)
            r.add_suffix(Gtk.Image(icon_name="folder-open-symbolic"))
            r.connect("activated", lambda *_: Gtk.FileLauncher(file=Gio.File.new_for_path(s.backup_done)).launch(self, None, None, None))
            g.add(r)
        rep = Adw.ActionRow(title=_("Salvar relatório"), activatable=True)
        rep.add_suffix(Gtk.Image(icon_name="document-save-symbolic"))
        rep.connect("activated", lambda *_: self.save_report())
        g.add(rep)
        page.add(g)
        return page

    def save_report(self):
        dlg = Gtk.FileDialog(initial_name=time.strftime("bigfoston-installer-%Y%m%d-%H%M%S.txt"))

        def done(d, res):
            try:
                f = d.save_finish(res)
            except GLib.Error:
                return
            s = self.state
            r = s.package.release if s.package else None
            text = "\n".join([f"BigFoston OS Installer {__version__}",
                              f"Versão: {r.version if r else '-'}", f"Cartão: {s.card.title if s.card else '-'}",
                              f"Backup: {s.backup_done or '-'}", "", *s.log]) + "\n"
            f.replace_contents(text.encode(), None, False, Gio.FileCreateFlags.REPLACE_DESTINATION, None)
            self.toast(_("Relatório salvo"))
        dlg.save(self, None, done)

    # ------------------------------------------------------------ verificar e reparar

    def repair_page(self):
        s = self.state
        page = Page(_("Verificação do cartão"), "repair")
        sp = page.add(Gtk.Spinner(spinning=True, halign=Gtk.Align.CENTER))
        result = Adw.PreferencesGroup()
        page.add(result)

        def work():
            mount = self.storage.mount(s.card)
            st = inst.card_state(mount)
            return st, inst.verify_card(self.storage, s.card, lambda step: None)

        def done(res):
            st, problems = res
            sp.set_visible(False)
            prep = st["prepared"]
            rows = [(_("Versão preparada"), prep["version"] if prep else _("nenhuma")),
                    (_("Agente no cartão"), _("sim") if st["agent"] else _("não")),
                    (_("Atualização pendente"), _("sim (o FOSTON aplica ao ligar)") if st["pending_update"] else _("não")),
                    (_("Último resultado no aparelho"), st["last_update"] or "—")]
            for t, v in rows:
                r = Adw.ActionRow(title=t, subtitle=v)
                r.set_subtitle_lines(0)
                result.add(r)
            if prep and not problems:
                page.add(Adw.StatusPage(icon_name="emblem-ok-symbolic", title=_("Tudo certo"),
                                        description=_("Os arquivos do cartão conferem com o que foi preparado.")))
            else:
                msg = "\n".join(problems) if problems else _("Não há uma versão preparada neste cartão.")
                page.add(Adw.StatusPage(icon_name="dialog-warning-symbolic", title=_("Problemas encontrados"),
                                        description=msg))
                b = Gtk.Button(label=_("Preparar o cartão de novo"), halign=Gtk.Align.CENTER)
                b.add_css_class("suggested-action")
                b.add_css_class("pill")

                def again(*_a):
                    s.mode = "update"
                    self.push(self.version_page())
                b.connect("clicked", again)
                page.add(b)
        run_bg(work, done, lambda e: (sp.set_visible(False), self.error(_("Não foi possível verificar"), e)))
        return page

    # ------------------------------------------------------------ restaurar

    def restore_page(self):
        page = Page(_("Restaurar o FOSTON original"), "restore")
        page.add(label(_("Escolha até onde voltar. O Windows CE, o bootloader e o firmware nunca são "
                         "alterados pelo BigFoston OS: só a pasta BigFoston e a entrada de início automático."),
                       ("dim-label",)))
        levels = (
            ("1", _("Usar o Windows CE agora"), _("Ligue o FOSTON e, na tela Dual Boot, toque em Windows CE. Nada é "
                                                 "apagado; da próxima vez a escolha aparece de novo.")),
            ("2", _("Desativar o BigFoston OS"), _("Na tela Dual Boot, toque em “Restaurar o FOSTON original”: o "
                                                  "agente desfaz o início automático, volta o caminho da navegação "
                                                  "guardado e mantém os arquivos no cartão para usar depois.")),
            ("3", _("Remover tudo"), _("Depois do nível 2, tire do cartão o que o instalador gravou (abaixo). Seus "
                                      "mapas, músicas e o navegador GPS continuam lá.")),
        )
        for n, t, d in levels:
            row = Gtk.Box(spacing=14)
            lb = Gtk.Label(label=n, valign=Gtk.Align.START)
            lb.add_css_class("step-number")
            row.append(lb)
            col = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
            col.append(label(t, ("heading",)))
            col.append(label(d))
            row.append(col)
            page.add(row)
        b = Gtk.Button(label=_("Tirar o BigFoston OS do cartão..."), halign=Gtk.Align.START)
        b.add_css_class("destructive-action")
        b.add_css_class("pill")
        b.connect("clicked", lambda *_: self.clean_card())
        b.set_sensitive(self.storage is not None)
        page.add(b)
        page.add(label(_("Se o FOSTON mostrar a porta serial em vez do pendrive no computador: Ajustes → USB → "
                         "MASS STORAGE. Guia completo: docs/17-recuperacao.md no GitHub."), ("dim-label", "caption")))
        return page

    def clean_card(self):
        def pick(cards):
            ok = [c for c in cards if not blockers(c) and c.has_bigfoston]
            if not ok:
                self.error(_("Nenhum cartão com BigFoston"), _("Coloque no leitor o cartão do FOSTON (ele precisa ter a "
                                                              "pasta BigFoston) e tente de novo."))
                return
            c = ok[0]
            d = Adw.AlertDialog(heading=_("Tirar o BigFoston OS de %s?") % f"{c.title} ({c.device})",
                                body=_("Remove do cartão só o agente, a atualização pendente e o install.json. "
                                       "Ficam: seus arquivos, a pasta BigFoston/data e os registros. Faça antes o "
                                       "nível 2 no aparelho, senão o FOSTON vai procurar o agente ao ligar."))
            d.add_response("cancel", _("Cancelar"))
            d.add_response("rm", _("Remover"))
            d.set_response_appearance("rm", Adw.ResponseAppearance.DESTRUCTIVE)

            def resp(_d, rid):
                if rid != "rm":
                    return

                def work():
                    card = self.storage.same_card(c)
                    removed = inst.remove_from_card(self.storage.mount(card))
                    self.storage.eject(card)
                    return removed
                run_bg(work, lambda rm: self.toast(_("Removido: ") + (", ".join(rm) or _("nada"))),
                       lambda e: self.error(_("Não foi possível limpar o cartão"), e))
            d.connect("response", resp)
            d.present(self)
        run_bg(self.storage.list_cards, pick, lambda e: self.error(_("Erro ao ler os discos"), e))
