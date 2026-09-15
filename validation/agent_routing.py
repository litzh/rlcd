import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from urllib.parse import parse_qs,urlsplit

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from rlcd.cli import Device

class Routing(unittest.TestCase):
    def test_sequence_is_queried_for_task_and_pet_is_forwarded(self):
        calls=[]
        class Local(Device):
            def request(self,path,data=None,method=None,content_type='application/json'):
                calls.append((path,data))
                if data is None: return json.dumps({'agent_id':'client-b','task_id':'build.42','seq':19}).encode()
                return data
        args=SimpleNamespace(agent='client-b',task='build.42',seq=None,state='working',progress=None,ttl=120,sound=False,pet='deepseek-whale',title=None,detail=None)
        result=Local('http://192.0.2.1').send(args)
        self.assertEqual(parse_qs(urlsplit(calls[0][0]).query),{'agent_id':['client-b'],'task_id':['build.42']})
        self.assertEqual(result['seq'],20)
        self.assertEqual(result['pet_id'],'deepseek-whale')
        self.assertIsNone(result['progress'])

if __name__=='__main__': unittest.main()
