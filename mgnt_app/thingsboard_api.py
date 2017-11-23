import requests

class thingsboard:
    def __init__(self, host, usr, pwd):
        self.host = 'http://' + host
        self.usr = usr
        self.pwd = pwd
        self.get_auth_token()

    def get_auth_token(self):
        payload = {"username": self.usr, "password": self.pwd}
        resp = requests.post(self.host + '/api/auth/login', json=payload)
        resp.raise_for_status()
        self.auth_header = {'X-Authorization': 'Bearer ' + resp.json()['token']}

    def get_request(self, path, parameters=None):
        resp = requests.get(self.host + path, headers=self.auth_header, params=parameters)
        return resp.text

    def post_request(self, path, body, parameters=None):
        resp = requests.post(self.host + path, headers=self.auth_header, params=parameters, json=body)
        return resp.text

    def delete_request(self, path, parameters=None):
        resp = requests.delete(self.host + path, headers=self.auth_header, params=parameters)
        return resp.text


if __name__ == "__main__":

    a = thingsboard('localhost:8080', 'siquigle@tcd.ie', 'admin')

    print(a.auth_header)
    print(a.get_request('/api/admin/updates'))
    print(a.post_request('/api/devices', {'deviceTypes': ['string']}))
