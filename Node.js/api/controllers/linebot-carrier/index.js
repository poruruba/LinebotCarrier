'use strict';

const config = {
  channelAccessToken: '【LINEチャネルアクセストークン（長期）】',
  channelSecret: '【LINEチャネルシークレット】',
};

const HELPER_BASE = process.env.HELPER_BASE || '../../helpers/';
const Response = require(HELPER_BASE + 'response');

var line_usr_id = '【LINEユーザID】';

const LineUtils = require(HELPER_BASE + 'line-utils');
const line = require('@line/bot-sdk');
const app = new LineUtils(line, config);

const speech = require('@google-cloud/speech');
const client = new speech.SpeechClient();

const mqtt = require('mqtt');
const MQTT_HOST = process.env.MQTT_HOST || '【MQTTサーバのURL(例：mqtt://hostname:1883)】';
const MQTT_CLIENT_ID = 'linebot-carrier';
const MQTT_TOPIC_TO_ATOM = 'linebot_to_atom';

const THIS_BASE_PATH = process.env.THIS_BASE_PATH;
const MESSAGE_MP3_FNAME	= THIS_BASE_PATH + '/public/message.mp3';

const fs = require('fs');

const AWS = require('aws-sdk');
const polly = new AWS.Polly({
  apiVersion: '2016-06-10',
  region: 'ap-northeast-1'
});

const mqtt_client = mqtt.connect(MQTT_HOST, { clientId: MQTT_CLIENT_ID });
mqtt_client.on('connect', () => {
  console.log("mqtt connected");
});

app.follow(async (event, client) =>{
  console.log("app.follow: " + event.source.userId );
//  line_usr_id = event.source.userId;
});

app.message(async (event, client) =>{
	console.log("linebot: app.message");

  var buffer = await speech_to_wave(event.message.text);
  fs.writeFileSync(MESSAGE_MP3_FNAME, buffer);

  var json = {
    message: event.message.text
  };
  mqtt_client.publish(MQTT_TOPIC_TO_ATOM, JSON.stringify(json));

	var message = {
		type: 'text',
		text: '$',
		emojis: [
			{
				index: 0,
				productId: "5ac1de17040ab15980c9b438",
				emojiId: 120
			}
		]
	};
  return client.replyMessage(event.replyToken, message);
});

exports.fulfillment = app.lambda();

exports.handler = async (event, context, callback) => {
  if( event.path == '/linebot-carrier-wav2text' ){
//    console.log(new Uint8Array(event.files['upfile'][0].buffer));
		var norm = normalize_wave8(new Uint8Array(event.files['upfile'][0].buffer));

		// 音声認識
		var result = await speech_recognize(norm);
		if( result.length < 1 )
			throw 'recognition failed';

    var text = result[0];
    console.log(text);
    app.client.pushMessage(line_usr_id, app.createSimpleResponse(text));
    return new Response({ message: text });
  }
};

function normalize_wave8(wav, out_bitlen = 16){
	var sum = 0;
	var max = 0;
	var min = 256;
	for( var i = 0 ; i < wav.length ; i++ ){
		var val = wav[i];
		if( val > max ) max = val;
		if( val < min ) min = val;
		sum += val;
	}

	var average = sum / wav.length;
	var amplitude = Math.max(max - average, average - min);
/*
	console.log('sum=' + sum);
	console.log('avg=' + average);
	console.log('amp=' + amplitude);
	console.log('max=' + max);
	console.log('min=' + min);
*/
	if( out_bitlen == 8 ){
		const norm = Buffer.alloc(wav.length);
		for( var i = 0 ; i < wav.length ; i++ ){
			var value = (wav[i] - average) / amplitude * (127 * 0.8) + 128;
			norm[i] = Math.floor(value);
		}
		return norm;
	}else{
		const norm = Buffer.alloc(wav.length * 2);
		for( var i = 0 ; i < wav.length ; i++ ){
			var value = (wav[i] - average) / amplitude * (32767 * 0.8);
			norm.writeInt16LE(Math.floor(value), i * 2);
		}
		return norm;
	}
}

async function speech_recognize(wav){
	const config = {
		encoding: 'LINEAR16',
		sampleRateHertz: 8192,
		languageCode: 'ja-JP',
	};
	const audio = {
		content: wav.toString('base64')
	};
	
	const request = {
		config: config,
		audio: audio,
	};	
	return client.recognize(request)
	.then(response =>{
		const transcription = [];
		for( var i = 0 ; i < response[0].results.length ; i++ )
			transcription.push(response[0].results[i].alternatives[0].transcript);

			return transcription;
	});
}

async function speech_to_wave(message, voiceid = 'Mizuki', samplerate = 16000 ){
	const pollyParams = {
    OutputFormat: 'mp3',
		Text: message,
		VoiceId: voiceid,
		TextType: 'text',
		SampleRate : String(samplerate),
	};
	
	return new Promise((resolve, reject) =>{
		polly.synthesizeSpeech(pollyParams, (err, data) =>{
			if( err ){
				console.log(err);
				return reject(err);
			}
			var buffer = Buffer.from(data.AudioStream);
			return resolve(buffer);
		});
	});
}