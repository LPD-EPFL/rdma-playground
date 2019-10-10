rdma_consensus_sources = [
    'consensus-protocol.c', 'leader-election.c', 'registry.c', 'utils.c',
    'ibv_layer.c', 'rdma-consensus.c', 'propose_api.c', 'parser.c'
]

cc = meson.get_compiler('c')

thread_dep = dependency('threads')
ibverbs_dep = cc.find_library('ibverbs', required : true)
rdmacm_dep = cc.find_library('rdmacm', required : true)
memcached_dep = cc.find_library('memcached', required : true)

rdmaconsensus_lib = shared_library('rdmaconsensus',
                        rdma_consensus_sources,
                        include_directories : inc,
                        dependencies : [thread_dep, ibverbs_dep,
                                        rdmacm_dep, memcached_dep],
                        link_with: [toml_lib],
                        install : true)

propose_test_sources = files('propose_main.c')
