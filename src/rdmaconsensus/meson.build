cc = meson.get_compiler('c')

rdma_consensus_sources = [
    files('consensus-protocol.c', 'leader-election.c', 'registry.c', 'utils.c',
    'ibv_layer.c', 'rdma-consensus.c', 'propose_api.c', 'parser.c')
]

thread_dep = dependency('threads')
ibverbs_dep = cc.find_library('ibverbs', required : true)
rdmacm_dep = cc.find_library('rdmacm', required : true)
memcached_dep = cc.find_library('memcached', required : true)
atomic_dep = cc.find_library('atomic', required : true)

rdmaconsensus_lib = shared_library('rdmaconsensus',
                        rdma_consensus_sources,
                        include_directories : inc,
                        dependencies : [thread_dep, atomic_dep, ibverbs_dep,
                                        rdmacm_dep, memcached_dep],
                        link_with: [toml_lib],
                        install : true)

tests_cpp_sources = files('../tests.cpp', 'consensus-protocol.c', 'leader-election.c', 'registry.c', 'utils.c',
    'ibv_layer.c', 'rdma-consensus.c', 'parser.c')
gtest_dep = cc.find_library('gtest', required : true)
tests_cpp_deps = [thread_dep, atomic_dep, ibverbs_dep,
                    rdmacm_dep, memcached_dep, gtest_dep]