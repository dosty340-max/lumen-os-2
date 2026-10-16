#!/usr/bin/env python3
"""Open Web (already in the app) and visit URLs, screenshotting each: web_visit.py name=url ..."""
import sys, time
sys.path.insert(0, __import__("os").path.dirname(__file__))
from ui import Phone
p = Phone()
for a in sys.argv[1:]:
    name, url = a.split("=", 1)
    p.tapL(150, 74); time.sleep(0.4); p.type(url + "\n")
    for i in range(40):
        time.sleep(0.5)
    p.shot("tmp/" + name)
