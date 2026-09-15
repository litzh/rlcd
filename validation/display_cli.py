from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'src'))
from rlcd import cli

class DisplayCommands(unittest.TestCase):
    def test_modes_and_status(self):
        for mode in ('on','off','toggle'):
            with patch.object(sys,'argv',['rlcd','display','invert',mode]), patch.object(cli.config,'read',return_value={'device':'http://192.0.2.1'}), patch.object(cli.Device,'request',return_value=b'{"inverted":true}') as request:
                cli.main()
                request.assert_called_once_with('/display/invert', ('{"mode": "'+mode+'"}').encode(),'PUT')
        with patch.object(sys,'argv',['rlcd','display','status']), patch.object(cli.config,'read',return_value={'device':'http://192.0.2.1'}), patch.object(cli.Device,'request',return_value=b'{"inverted":false}') as request:
            cli.main()
            request.assert_called_once_with('/display')

if __name__=='__main__': unittest.main()
