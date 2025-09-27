To run the code run the following instructions

To set up the third party to get correlated randomness (this is not required for writing)
```
python3 share_server.py --listen 0.0.0.0:9300
```

Set up the two servers
```
python3 party_client.py --role A --rows 100   --listen 0.0.0.0:9700 --peer-listen 9701   --peer 127.0.0.1:9801 --share 127.0.0.1:9300
```
```
python3 party_client.py --role B --rows 100   --listen 0.0.0.0:9800 --peer-listen 9801   --peer 127.0.0.1:9701 --share 127.0.0.1:9300
```

To read
```
python3 coordinator.py --op read --dim 10 --idx 5   --c0 0.0.0.0:9700 --c1 0.0.0.0:9800
```
To write

```
python3 coordinator.py --op write --dim 10 --idx 5 --val 1   --c0 0.0.0.0:9700 --c1 0.0.0.0:9800
```

