"""This carved umi, not a model-wide partition layout. Units are 4096 bytes."""
BS = 4096
DISK_BLOCKS = 30945280
GPT_INDEX = 34                       # zero-based; Linux partition number is 35
PARTN = GPT_INDEX + 1
FIRST, LAST = 14167552, 30945274
BLOCKS = LAST - FIRST + 1
TYPE = '7c3457ef-0000-11aa-aa11-00306543ecac'
UNIQUE = 'd8795ba0-8d83-4cd0-8d8b-03186f8b422f'
DEV, NODE = '/dev/block/by-name/darwin', '/dev/block/sda35'
ROOT_BLOCKS, OS_BLOCKS, APP_BLOCKS = 1374208, 665600, 2560
DATA_FIRST = ROOT_BLOCKS + OS_BLOCKS + APP_BLOCKS  # loader/kernel both use 2042368
DATA_BLOCKS = BLOCKS - DATA_FIRST
SLOTS = {'root': (0, ROOT_BLOCKS), 'os': (ROOT_BLOCKS, OS_BLOCKS),
         'app': (ROOT_BLOCKS + OS_BLOCKS, APP_BLOCKS), 'data': (DATA_FIRST, DATA_BLOCKS)}
