import os
import time
from datetime import timedelta

import pytest

from test_cert import Credentials
from test_env import TlsTestEnv, ExecResult
from test_conf import TlsTestConf


@pytest.mark.skip(reason="client certs disabled")
class TestTLS:

    env = TlsTestEnv()

    @classmethod
    def setup_class(cls):
        if cls.env.is_live(timeout=timedelta(milliseconds=100)):
            assert cls.env.apache_stop() == 0
        cls.clientsX = cls.env.CA.get_first("clientsX")
        cls.clientsY = cls.env.CA.get_first("clientsY")
        cls.cax_file = os.path.join(os.path.dirname(cls.clientsX.cert_file), "clientX-ca.pem")
        with open(cls.cax_file, 'w') as fd:
            fd.write("".join(open(cls.clientsX.cert_file).readlines()))
            fd.write("".join(open(cls.env.CA.cert_file).readlines()))

    @classmethod
    def teardown_class(cls):
        if cls.env.is_live(timeout=timedelta(milliseconds=100)):
            assert cls.env.apache_stop() == 0

    def setup_method(self, _method):
        if self.env.is_live(timeout=timedelta(milliseconds=100)):
            assert self.env.apache_stop() == 0

    def get_ssl_var(self, domain: str, cert: Credentials, name: str):
        r = self.env.https_get(domain, f"/vars.py?name={name}", extra_args=[
            "--cert", cert.cert_file
        ] if cert else [])
        assert r.exit_code == 0, r.stderr
        assert r.json, r.stderr + r.stdout
        return r.json[name] if name in r.json else None

    def test_12_set_ca_non_existing(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_a, self.env.domain_b], extras={
            self.env.domain_a: """
            TLSClientCA xxx 
            """
        })
        conf.write()
        assert self.env.apache_restart() == 1

    def test_12_set_ca_existing(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_a, self.env.domain_b], extras={
            self.env.domain_a: f"""
            TLSClientCA {self.cax_file}
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0

    def test_12_set_auth_no_ca(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_a, self.env.domain_b], extras={
            self.env.domain_a: """
            TLSClientCertificate required
            """
        })
        conf.write()
        # will fail bc lacking clien CA
        assert self.env.apache_restart() == 1

    def test_12_auth_option_std(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_b], extras={
            self.env.domain_b: f"""
            TLSClientCertificate required
            TLSClientCA {self.cax_file}
            # TODO: TLSUserName SSL_CLIENT_S_DN_CN 
            TLSOptions +StdEnvVars
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        # should be denied
        r = self.env.https_get(domain=self.env.domain_b, paths="/index.json")
        assert r.exit_code != 0, r.stdout
        # should work
        ccert = self.clientsX.get_first("user1")
        data = self.env.https_get_json(self.env.domain_b, "/index.json", extra_args=[
            "--cert", ccert.cert_file
        ])
        assert data == {'domain': self.env.domain_b}
        r = self.env.https_get(self.env.domain_b, "/vars.py?name=SSL_CLIENT_S_DN_CN")
        assert r.exit_code != 0, "should have been prevented"
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_S_DN_CN")
        assert val == 'Not Implemented'
        #TODO
        #val = self.get_ssl_var(self.env.domain_b, ccert, "REMOTE_USER")
        #assert val == 'Not Implemented'
        # not set on StdEnvVars, needs option ExportCertData
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_CERT")
        assert val == ""

    def test_12_auth_option_cert(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_b], extras={
            self.env.domain_b: f"""
            TLSClientCertificate required
            TLSClientCA {self.cax_file}
            TLSOptions Defaults +ExportCertData
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        ccert = self.clientsX.get_first("user1")
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_CERT")
        assert val == ccert.cert_pem.decode()
        # no chain should be present
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_CHAIN_0")
        assert val == ''
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_SERVER_CERT")
        assert val
        server_certs = self.env.get_certs_for(self.env.domain_b)
        assert val in [c.cert_pem.decode() for c in server_certs]

    def test_12_auth_ssl_optional(self):
        conf = TlsTestConf(env=self.env)
        domain = self.env.domain_b
        conf.add_ssl_vhosts(domains=[domain], extras={
            domain: f"""
            SSLVerifyClient optional
            SSLVerifyDepth 2
            SSLOptions +StdEnvVars +ExportCertData
            SSLCACertificateFile {self.cax_file}
            SSLUserName SSL_CLIENT_S_DN
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        # should work either way
        data = self.env.https_get_json(domain, "/index.json")
        assert data == {'domain': domain}
        # no client cert given, we expect the server variable to be empty
        val = self.get_ssl_var(self.env.domain_b, None, "SSL_CLIENT_S_DN_CN")
        assert val == ''
        ccert = self.clientsX.get_first("user1")
        data = self.env.https_get_json(domain, "/index.json", extra_args=[
            "--cert", ccert.cert_file
        ])
        assert data == {'domain': domain}
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_S_DN_CN")
        assert val == 'user1'
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_S_DN")
        assert val == 'O=abetterinternet-mod_tls,OU=clientsX,CN=user1'
        val = self.get_ssl_var(self.env.domain_b, ccert, "REMOTE_USER")
        assert val == 'O=abetterinternet-mod_tls,OU=clientsX,CN=user1'
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_I_DN")
        assert val == 'O=abetterinternet-mod_tls,OU=clientsX'
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_I_DN_CN")
        assert val == ''
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_I_DN_OU")
        assert val == 'clientsX'
        val = self.get_ssl_var(self.env.domain_b, ccert, "SSL_CLIENT_CERT")
        assert val == ccert.cert_pem.decode()

    def test_12_auth_optional(self):
        conf = TlsTestConf(env=self.env)
        domain = self.env.domain_b
        conf.add_md_vhosts(domains=[domain], extras={
            domain: f"""
            TLSClientCertificate optional
            TLSClientCA {self.cax_file}
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        # should work either way
        data = self.env.https_get_json(domain, "/index.json")
        assert data == {'domain': domain}
        # no client cert given, we expect the server variable to be empty
        r = self.env.https_get(domain, "/vars.py?name=SSL_CLIENT_S_DN_CN")
        assert r.exit_code == 0, r.stderr
        assert r.json == {
            'SSL_CLIENT_S_DN_CN': '',
        }, r.stdout
        data = self.env.https_get_json(domain, "/index.json", extra_args=[
            "--cert", self.clientsX.get_first("user1").cert_file
        ])
        assert data == {'domain': domain}
        r = self.env.https_get(domain, "/vars.py?name=SSL_CLIENT_S_DN_CN", extra_args=[
            "--cert", self.clientsX.get_first("user1").cert_file
        ])
        # with client cert, we expect the server variable to show? Do we?
        assert r.exit_code == 0, r.stderr
        assert r.json == {
            'SSL_CLIENT_S_DN_CN': 'Not Implemented',
        }, r.stdout

    def test_12_auth_expired(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_b], extras={
            self.env.domain_b: f"""
            TLSClientCertificate required
            TLSClientCA {self.cax_file}
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        # should not work
        r = self.env.https_get(domain=self.env.domain_b, paths="/index.json", extra_args=[
            "--cert", self.clientsX.get_first("user_expired").cert_file
        ])
        assert r.exit_code != 0

    def test_12_auth_other_ca(self):
        conf = TlsTestConf(env=self.env)
        conf.add_md_vhosts(domains=[self.env.domain_b], extras={
            self.env.domain_b: f"""
            TLSClientCertificate required
            TLSClientCA {self.cax_file}
            """
        })
        conf.write()
        assert self.env.apache_restart() == 0
        # should not work
        r = self.env.https_get(domain=self.env.domain_b, paths="/index.json", extra_args=[
            "--cert", self.clientsY.get_first("user1").cert_file
        ])
        assert r.exit_code != 0
        # This will work, as the CA root is present in the CA file
        r = self.env.https_get(domain=self.env.domain_b, paths="/index.json", extra_args=[
            "--cert", self.env.CA.get_first("user1").cert_file
        ])
        assert r.exit_code == 0
