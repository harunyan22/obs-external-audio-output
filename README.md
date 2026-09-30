# External Audio Output for OBS Studio

OBS Studio の音声(音声トラック)を、OBS のモニタリング機能とは**独立して**、任意の再生デバイスへ低遅延で出力する Windows 用プラグインです。

Outputs an OBS audio track to any playback device on Windows, independently of OBS's built-in audio monitoring. Japanese/English UI included.

```
USB マイク ─┐
            ├→ OBS(音声トラック 6)→ 本プラグイン → 任意の出力デバイス(AUX / USB DAC / HDMI …)
PC 上の BGM ─┘                                       → 接続機材を介してスマートフォン・別 PC・外部機器へ
```

## スマホ向け配信アプリでの活用

IRIAM などのスマホ向け配信アプリに、OBS でまとめたマイク・BGM などの音声を送る用途にも活用できます。PC 側で音量や音声トラックを調整し、自分用のヘッドホンモニタリングとスマホへ送る音声を分けられます。

スマホへ音声を送るには、**スマホの外部音声入力に対応したオーディオインターフェースやミキサー、接続ケーブル、端子に合った変換アダプタなどが別途必要**です。必要な機材はスマホの機種・端子(USB-C / Lightning など)と接続方法によって異なります。変換アダプタは端子の形だけでなく、音声入力に対応していることも確認してください。

接続のイメージ: `OBS → 本プラグイン → PC の音声出力 → スマホ対応の音声入力機材 → スマホの配信アプリ`

本プラグイン単体でスマホへ音声を転送したり、IRIAM に直接接続したりする機能はありません。利用にはアプリが接続機材からの外部音声入力を扱えることが必要です。IRIAM を含む各アプリ・機材の組み合わせでの動作確認は行っていません。

## 特徴
- OBS 本来のモニタリング(ヘッドホン確認)と**同時に使える**独立出力(WASAPI 共有モード)
- ドック UI: 出力先の選択、開始/停止、レベルメーター、状態表示
- デバイスの切断・スリープ・既定デバイス変更からの自動復帰
- 設定は自動保存され、OBS 起動時に自動で出力再開
- 追加遅延はログからの推定平均 約 52ms(確認環境での目安。物理的な遅延の実測値ではありません)
- 実験的: OBS モニタリング互換モード(相性問題時の代替手段)

## 動作環境
- Windows 11(動作確認済み)/ Windows 10(コード上の制約はありませんが**未検証**)
- OBS Studio 32.2.1 x64 で動作確認(ほかのバージョンは未検証)

## インストール
1. OBS を終了し、Releases から `obs-external-audio-output-0.1.0-windows-x64.zip` をダウンロードして展開
2. `obs-external-audio-output` フォルダを次の場所に配置:
   `C:\ProgramData\obs-studio\plugins\`
   (最終的に `C:\ProgramData\obs-studio\plugins\obs-external-audio-output\bin\64bit\obs-external-audio-output.dll` となる形)
3. OBS Studio を再起動

> 本プラグインはコード署名していないため、ダウンロード時に SmartScreen 等の警告が出ることがあります。

## 使い方
1. OBS のドック「外部音声出力」を表示(メニュー「ドック」から)
2. 「出力先」でデバイスを選び、「出力開始」を押す — これだけです
3. 既定では**音声トラック 6**(OBS の標準設定では全ソースが含まれます)を出力します。特定のソースだけを出したい場合は、OBS の「詳細音声プロパティ」でトラック 6 の ON/OFF を調整してください
4. 細かい設定(トラック番号・バッファ・出力方式)は「詳細設定」にあります

- 初回インストール直後は出力停止状態です。一度「出力開始」すると、以後は OBS 起動時に自動で開始します。
- 通常の独立出力モードは、配信・録画とは独立した取り出し口から音声を出力します。

## アンインストール
1. 互換モードを使用している場合は、先に独立出力モードへ戻してください。OBS を終了し、`C:\ProgramData\obs-studio\plugins\obs-external-audio-output` を削除
2. 設定ファイルも消す場合: `%APPDATA%\obs-studio\plugin_config\obs-external-audio-output` を削除

## 既知の制限
- Windows 専用・出力先は 1 デバイス(排他モード非対応、共有モードのみ)
- 「OBS モニタリング互換」モードは実験的機能です(有効中は OBS のモニタリング先と対象ソースのモニタリング設定を一時的に変更します。無効化時に復元し、終了時に残るソース設定の復元は次回起動時に行います)

## ライセンス
GPL-2.0-or-later。ソースコードは本リポジトリで公開されています。ビルド方法は [BUILDING.md](BUILDING.md) を参照してください。

Based on [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
