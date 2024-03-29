import time
from datetime import timedelta

from test_env import TlsTestEnv, ExecResult
from test_conf import TlsTestConf


class TestMD:

    env = TlsTestEnv()

    @classmethod
    def setup_class(cls):
        conf = TlsTestConf(env=cls.env)
        conf.add_md_vhosts(domains=[cls.env.domain_a, cls.env.domain_b], extras={
            'base': """
            LogLevel tls:trace4 md:trace4
            """
        })
        conf.write()
        assert cls.env.apache_restart() == 0

    @classmethod
    def teardown_class(cls):
        if cls.env.is_live(timeout=timedelta(milliseconds=100)):
            assert cls.env.apache_stop() == 0

    def test_11_get_a(self):
        # do we see the correct json for the domain_a?
        data = self.env.https_get_json(self.env.domain_a, "/index.json")
        assert data == {'domain': self.env.domain_a}

    def test_11_get_b(self):
        # do we see the correct json for the domain_a?
        data = self.env.https_get_json(self.env.domain_b, "/index.json")
        assert data == {'domain': self.env.domain_b}

    def test_11_get_base(self):
        # give the base server domain_a and lookup its index.json
        conf = TlsTestConf(env=self.env)
        conf.add_md_base(domain=self.env.domain_a)
        conf.write()
        assert self.env.apache_restart() == 0
        data = self.env.https_get_json(self.env.domain_a, "/index.json")
        assert data == {'domain': 'localhost'}
