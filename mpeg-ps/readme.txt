https://github.com/ireader/media-server/tree/master/libmpeg

ffplay rec.g711a -f alaw -ar 8000 -ac 1
ffplay av711.mpg -vn
ffplay av711.mpg
ffplay av_aac.mpg -vn
ffplay av_aac.mpg
ffplay audio.aac

这里的轮流读取音频帧和视频帧的方式是错误的，因为时间戳对不齐，有些播放器会根据时间戳判断是否错误并丢弃
MP4里将视频帧和音频帧按时间戳排序写入MP4才是对的
