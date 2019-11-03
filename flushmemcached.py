from pymemcache.client import base

# Don't forget to run `memcached' before running this next line:
client = base.Client(('localhost', 11211))
client.flush_all()


