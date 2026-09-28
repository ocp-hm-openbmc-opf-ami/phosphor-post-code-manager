gtest_dep = dependency('gtest', main: false, required: false)
gmock_dep = dependency('gmock', required: false)

if not gtest_dep.found() or not gmock_dep.found()
    cmake = import('cmake')
    opt_var = cmake.subproject_options()
    opt_var.add_cmake_defines({'CMAKE_CXX_FLAGS': '-Wno-pedantic'})
    opt_var.set_override_option('warning_level', '3')
    opt_var.set_override_option('werror', 'true')

    gtest_proj = cmake.subproject('gtest', options: opt_var, required: true)
    if gtest_proj.found()
        gtest_dep = declare_dependency(
            dependencies: [dependency('threads'), gtest_proj.dependency('gtest')]
        )
        gmock_dep = gtest_proj.dependency('gmock')
    else
        assert(
            not get_option('tests').enabled(),
            'Googletest is required when tests are enabled',
        )
    endif
endif

post_code_sources = [
    '../src/post_code.cpp',
]

post_code_includes = include_directories('..', '../inc')

test(
    'post_code_test',
    executable(
        'post_code_test',
        'test_main.cpp',
        'test_post_code.cpp',
        post_code_sources,
        include_directories: post_code_includes,
        implicit_include_directories: false,
        dependencies: [
            gtest_dep,
            gmock_dep,
            sdbusplus,
            phosphor_dbus_interfaces,
            phosphor_logging,
            cereal_dep,
        ],
        # Suppress unused-result for std::system() used in test GlobalEnvironment.
        # This flag applies only to the test binary, not production targets.
        cpp_args: ['-Wno-unused-result'],
    ),
)
