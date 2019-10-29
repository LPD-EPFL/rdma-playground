project('Dory RDMA consensus', 'c', 'cpp',
    default_options : ['c_std=c11', 'cpp_std=c++14', 'warning_level=3', 'buildtype=debugoptimized'],
    version : '1.0.0',
    license : 'MIT')

add_global_arguments(['-Wno-unused-function',
                      '-Wno-pedantic'],
                     language: ['c', 'cpp'])

# add_global_arguments('-DCONSENSUS_UC', language : ['c', 'cpp'])
add_global_arguments('-DCONSENSUS_INLINE', language : ['c', 'cpp'])
add_global_arguments('-DCONSENSUS_PIN', language : ['c', 'cpp'])
add_global_arguments('-DCONSENSUS_SIGNAL', language : ['c', 'cpp'])


# Used for linux CPU affinity
add_global_arguments('-D_GNU_SOURCE', language : ['c', 'cpp'])

inc = [include_directories('include')]

# add_global_arguments('-DSOME_TOKEN=value', language : 'cpp')
# glib_dep = dependency('glib-2.0')


subdir('include')
subdir('third_party')

subdir('src')
# subdir('test')


executable('propose-test',
    propose_test_sources,
    include_directories : inc,
    link_with : rdmaconsensus_lib
)

executable('redis-latency-test',
    redis_latency_test_sources,
    include_directories : inc,
    dependencies : [dependency('threads')]
)

# executable('tests-c',
#     tests_c_sources,
#     include_directories : inc,
#     link_with : rdmaconsensus_lib
# )

executable('tests-cpp',
    tests_cpp_sources,
    include_directories : inc,
    dependencies : tests_cpp_deps,
    link_with : [toml_lib]
)

# pkg_mod = import('pkgconfig')
# pkg_mod.generate(libraries : foolib,
#                  version : '1.0',
#                  name : 'libfoobar',
#                  filebase : 'foobar',
#                  description : 'A Library to barnicate your foos.')
