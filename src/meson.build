subdir('helpers')

# gtkwave

vcd_keywords_c = custom_target(
    'vcd_keywords.cpp',
    input: 'vcd_keywords.gperf',
    output: 'vcd_keywords.cpp',
    command: [
        gperf,
        '-i', '1',
        '-L', 'ANSI-C',
        '-C',
        '-k', '1,\044',
        '-H', 'keyword_hash',
        '-N', 'check_identifier',
        '-tT',
        '--initializer-suffix=,0',
        '--output-file', '@OUTPUT@',
        '@INPUT@',
    ],
)

gtkwave_resources = gnome.compile_resources(
    'gtkwave-resources',
    'io.github.gtkwave.GTKWave.gresource.xml',
    c_name: 'gtkwave',
)

gtkwave_sources = [
    'analyzer.cpp',
    'baseconvert.cpp',
    'bitvec.cpp',
    'bsearch.cpp',
    'busy.cpp',
    'clipping.cpp',
    'currenttime.cpp',
    'debug.cpp',
    'edgebuttons.cpp',
    'entry.cpp',
    'dump_file_main.cpp',
    'fetchbuttons.cpp',
    'fgetdynamic.cpp',
    'file.cpp',
    'fonts.cpp',
    'gconf.cpp',
    'globals.cpp',
    'gtk23compat.cpp',
    'gw-fst-file.cpp',
    'gw-fst-loader.cpp',
    'gw-mouseover.cpp',
    'gw-named-marker-dialog.cpp',
    'gw-time-display.cpp',
    'gw-vcd-file.cpp',
    'gw-vcd-loader.cpp',
    'gw-wave-view-traces.cpp',
    'gw-wave-view.cpp',
    'logfile.cpp',
    'lx2.cpp',
    'main.cpp',
    'markerbox.cpp',
    'menu.cpp',
    'mouseover.cpp',
    'pagebuttons.cpp',
    'pipeio.cpp',
    'pixmaps.cpp',
    'print.cpp',
    'ptranslate.cpp',
    'rc.cpp',
    'regex.cpp',
    'renderopt.cpp',
    'savefile.cpp',
    'search.cpp',
    'shiftbuttons.cpp',
    'showchange.cpp',
    'signal_list.cpp',
    'signalwindow.cpp',
    'simplereq.cpp',
    'splash.cpp',
    'status.cpp',
    'strace.cpp',
    'symbol.cpp',
    'tcl_commands.cpp',
    'tcl_helper.cpp',
    'tcl_np.cpp',
    'tcl_support_commands.cpp',
    'timeentry.cpp',
    'translate.cpp',
    'tree_component.cpp',
    'tree.cpp',
    'treesearch.cpp',
    'ttranslate.cpp',
    'vcd_saver.cpp',
    'vcd.cpp',
    'vlist.cpp',
    'wavewindow.cpp',
    'zoombuttons.cpp',
    'cocoa/cocoa_misc.cpp',
]

gtkwave_dependencies = [
    glib_dep,
    gtk_dep,
    gtk_mac_integration_dep,
    gtk_unix_print_dep,
    m_dep,
    libfst_dep,
    libjrb_dep,
    bzip2_dep,
    judy_dep,
    tcl_dep,
    tk_dep,
    gnu_regex_dep,
    libgtkwave_dep,
]

gtkwave_include_directories = [
    config_inc,
    'cocoa',
]

executable(
    'gtkwave',
    gtkwave_sources,
    gtkwave_resources,
    vcd_keywords_c,
    dependencies: gtkwave_dependencies,
    include_directories: gtkwave_include_directories,
    c_args: warning_args,
    link_args: link_args,
    install: true,
    install_rpath: install_rpath,
)

# twinwave

twinwave_sources = [
    'twinwave.cpp',
]

twinwave_dependencies = [
    gtk_dep,
    libgtkwave_dep,
]

twinwave_include_directories = [
    config_inc,
    # 'cocoa',
]

executable(
    'twinwave',
    twinwave_sources,
    dependencies: twinwave_dependencies,
    include_directories: twinwave_include_directories,
    install: true,
    install_rpath: install_rpath,
)
