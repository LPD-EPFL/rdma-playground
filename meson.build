project('Dory RDMA consensus', 'c', 'cpp',
    default_options : ['c_std=c11', 'cpp_std=c++14', 'warning_level=3', 'buildtype=debugoptimized'],
    version : '1.0.0',
    license : 'MIT')

# add_global_arguments('-DGNU_SOURCE', language : 'c')

inc = [include_directories('include')]

# add_global_arguments('-DSOME_TOKEN=value', language : 'cpp')
# glib_dep = dependency('glib-2.0')


subdir('include')
subdir('third_party')

subdir('src')
# subdir('test')

sample_cpp_project_bin = executable('propose-test',
    propose_test_sources,
    include_directories : inc,
    link_with : rdmaconsensus_lib
)

# pkg_mod = import('pkgconfig')
# pkg_mod.generate(libraries : foolib,
#                  version : '1.0',
#                  name : 'libfoobar',
#                  filebase : 'foobar',
#                  description : 'A Library to barnicate your foos.')
