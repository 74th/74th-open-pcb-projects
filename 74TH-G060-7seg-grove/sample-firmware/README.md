# Sample firmware for M5Stack Atom Lite

Movie tweet: https://x.com/74th/status/1870251968486355227

ボタンを押すたびに以下の動作を切り替えます。

1. ぐるぐる
2. ライン移動
3. カウントアップ
4. 74th

## バイナリを直接書き込む方法

Web Toolを使って書き込む場合は、以下のURLにアクセスし、M5StackAtomをPCに接続した状態で、「7Seg Grove Sample for M5Stack Atom Lite」のConnectボタンを押し、デバイスの選択でM5Stackを選択し、インストールを進めてください。

https://74th.github.io/74th-oshw-projects/esp-web-tool.html

CLIツールを使う場合、[esptool.py](https://github.com/espressif/esptool)、もしくは [espflash](https://github.com/esp-rs/espflash) を使い、以下のコマンドを実行してください。

```
esptool.py --chip esp32 write_flash -z 0 firmware.bin
espflash write-bin 0 firmware.bin
```
## PlatformIOでのビルド方法

- VS Codeをインストールし、拡張機能PlatformIO IDEをインストールします。
- sample-firmwareフォルダをVS Codeで開きます。
- F1キーで開くコマンドパレットで、コマンド"PlatformIO: Upload"を実行する