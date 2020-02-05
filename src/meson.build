fprintd_marshal = gnome.genmarshal('fprintd-marshal',
    prefix: 'fprintd_marshal',
    sources: 'fprintd-marshal.list',
    valist_marshallers: true,
)

bash = find_program('bash')
dbus_binding_tool = find_program('dbus-binding-tool')
dbus_interfaces = ['Manager', 'Device']
dbus_interfaces_files = []
dbus_server_glue_sources = []

foreach interface_name: dbus_interfaces
    interface = interface_name.to_lower()
    interface_file = interface + '.xml'
    glue_name = interface + '-dbus-glue.h'
    dbus_server_glue_sources += custom_target(glue_name,
        input: interface_file,
        output: glue_name,
        command: [
            dbus_binding_tool,
            '--prefix=fprint_' + interface,
            '--mode=glib-server',
            '--output=@OUTPUT@',
            '@INPUT@',
        ])

    dbus_interfaces_files += custom_target('dbus_interface_' + interface,
        input: interface_file,
        output: 'net.reactivated.Fprint.@0@.xml'.format(interface_name),
        command: ['cp', '@INPUT@', '@OUTPUT@'],
        install: true,
        install_dir: dbus_interfaces_dir,
    )
endforeach

fprintd_deps = declare_dependency(
    include_directories: [
        include_directories('..'),
    ],
    dependencies: [
        dbus_glib_dep,
        glib_dep,
        gio_dep,
        gmodule_dep,
        libfprint_dep,
        polkit_gobject_dep,
    ],
    compile_args: [
        '-DG_LOG_DOMAIN="@0@"'.format(meson.project_name()),
        '-DLOCALEDIR="@0@"'.format(localedir),
        '-DPLUGINDIR="@0@"'.format(fprintd_plugindir),
    ],
)

symbol_visibility = get_option('b_coverage') ? 'default' : 'hidden'

libfprintd_private = static_library('fprintd-private',
    sources: [
        'device.c',
        'fprintd.h',
        'manager.c',
        dbus_server_glue_sources,
        fprintd_marshal,
    ],
    dependencies: fprintd_deps,
    gnu_symbol_visibility: symbol_visibility,
)

fprintd = executable('fprintd',
    sources: [
        'file_storage.c',
        'file_storage.h',
        'fprintd.h',
        'main.c',
        'storage.h',
    ],
    dependencies: fprintd_deps,
    link_with: libfprintd_private,
    gnu_symbol_visibility: symbol_visibility,
    install: true,
    install_dir: fprintd_installdir,
)
