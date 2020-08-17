dbus_client_glue_sources = []

foreach interface_name: dbus_interfaces
    interface = interface_name.to_lower()
    interface_file = meson.source_root() / 'src' / interface + '.xml'
    glue_name = interface + '-dbus-glue.h'
    dbus_client_glue_sources += custom_target(glue_name,
        input: interface_file,
        output: glue_name,
        command: [
            dbus_binding_tool,
            '--prefix=fprint_' + interface,
            '--mode=glib-client',
            '--output=@OUTPUT@',
            '@INPUT@',
        ])
endforeach

libfprintd_utils_dep = declare_dependency(
    include_directories: [
        include_directories('../pam'),
    ],
    dependencies: [
        glib_dep,
        dbus_glib_dep,
    ],
    sources: [
        fprintd_marshal,
        dbus_client_glue_sources,
    ],
    link_with: static_library('fprintd_utils',
        sources: [
            dbus_client_glue_sources,
            fprintd_marshal,
        ],
        dependencies: [
            glib_dep,
        ]
    ),
)

utils = [
    'delete',
    'enroll',
    'list',
    'verify',
]

fprintd_utils = []

foreach util: utils
    fprintd_utils += executable('fprintd-' + util,
        sources: util + '.c',
        dependencies: libfprintd_utils_dep,
        install: true,
    )
endforeach
