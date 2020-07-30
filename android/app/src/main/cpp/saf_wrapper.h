/*
 * Copyright (c) 2020 Taner Sener
 *
 * This file is part of MobileFFmpeg.
 *
 * MobileFFmpeg is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MobileFFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MobileFFmpeg.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MOBILE_FFMPEG_SAF_WRAPPER_H
#define MOBILE_FFMPEG_SAF_WRAPPER_H

/*
 *  These wrappers are intended to be used instead of the ffmpeg apis.
 *  You don't even need to change the source to call them.
 *  Instead, we redefine the public api names so that the wrapper be used.
 */

int android_avio_closep(AVIOContext **s);
#define avio_closep android_avio_closep

void android_avformat_close_input(AVFormatContext **s);
#define avformat_close_input android_avformat_close_input

int android_avio_open(AVIOContext **s, const char *url, int flags);
#define avio_open android_avio_open

int android_avformat_open_input(AVFormatContext **ps, const char *filename,
                        ff_const59 AVInputFormat *fmt, AVDictionary **options);
#define avformat_open_input android_avformat_open_input

// workaround for https://issuetracker.google.com/issues/162440528
// ANDROID_CREATE_DOCUMENT generating file names like "transcode.mp3 (2)"
AVOutputFormat *fix_guess_format(const char *short_name,
                                const char *filename,
                                const char *mime_type);
#define av_guess_format fix_guess_format

#endif //MOBILE_FFMPEG_SAF_WRAPPER_H
