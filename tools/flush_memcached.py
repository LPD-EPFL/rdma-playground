#!/usr/bin/env python3

# Install pymemcache with 
# pip3 install --user pymemcache

from pymemcache.client import base

client = base.Client(('127.0.0.1', 11211))
client.flush_all()
