/*
 * Copyright (c) 2018 Taner Sener
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
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with MobileFFmpeg.  If not, see <http://www.gnu.org/licenses/>.
 */

package com.arthenica.mobileffmpeg.test;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.text.method.ScrollingMovementMethod;
import android.util.AndroidRuntimeException;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.TextView;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.fragment.app.Fragment;

import com.arthenica.mobileffmpeg.Config;
import com.arthenica.mobileffmpeg.FFprobe;
import com.arthenica.mobileffmpeg.LogCallback;
import com.arthenica.mobileffmpeg.LogMessage;

import java.util.concurrent.Callable;

import static android.app.Activity.RESULT_OK;

public class ScopedStorageTabFragment extends Fragment {

    private EditText commandText;
    private TextView outputText;
    private Uri inUri;
    private Uri outUri;
    private static final int REQUEST_SAF_FFPROBE = 11;
    private static final int REQUEST_SAF_TRANSCODE_IN = 12;
    private static final int REQUEST_SAF_TRANSCODE_OUT = 13;

    public ScopedStorageTabFragment() {
        super(R.layout.fragment_command_tab);
    }

    @Override
    public void onViewCreated(@NonNull View view, @Nullable Bundle savedInstanceState) {
        super.onViewCreated(view, savedInstanceState);

        commandText = view.findViewById(R.id.commandText);
        commandText.setVisibility(View.GONE);

        Button runFFmpegButton = view.findViewById(R.id.runFFmpegButton);
        runFFmpegButton.setText(R.string.command_run_transcode_button_text);
        runFFmpegButton.setOnClickListener(new View.OnClickListener() {

            @Override
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .setType("*/*")
                        .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"video/*", "audio/*"})
                        .addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(intent, REQUEST_SAF_TRANSCODE_IN);
            }
        });

        Button runFFprobeButton = view.findViewById(R.id.runFFprobeButton);
        runFFprobeButton.setOnClickListener(new View.OnClickListener() {

            @Override
            public void onClick(View v) {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT)
                        .setType("*/*")
                        .putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"image/*", "video/*", "audio/*"})
                        .addCategory(Intent.CATEGORY_OPENABLE);
                startActivityForResult(intent, REQUEST_SAF_FFPROBE);
            }
        });

        outputText = view.findViewById(R.id.outputText);
        outputText.setMovementMethod(new ScrollingMovementMethod());

        Log.d(MainActivity.TAG, "Last command output was: " + Config.getLastCommandOutput());
    }

    @Override
    public void onResume() {
        super.onResume();
        setActive();
    }

    public static ScopedStorageTabFragment newInstance() {
        return new ScopedStorageTabFragment();
    }

    public void enableLogCallback() {
        Config.enableLogCallback(new LogCallback() {

            @Override
            public void apply(final LogMessage message) {
                MainActivity.addUIAction(new Callable() {

                    @Override
                    public Object call() {
                        appendLog(message.getText());
                        return null;
                    }
                });

                throw new AndroidRuntimeException("I am test exception thrown by test application");
            }
        });
    }

    private void runFFprobe() {
        clearLog();

        final String ffprobeCommand = "-hide_banner " + Config.getCommandParameter(getContext(), inUri);

        Log.d(MainActivity.TAG, "Testing FFprobe COMMAND synchronously.");

        Log.d(MainActivity.TAG, String.format("FFprobe process started with arguments\n\'%s\'", ffprobeCommand));

        int result = FFprobe.execute(ffprobeCommand);

        Log.d(MainActivity.TAG, String.format("FFprobe process exited with rc %d", result));

        if (result != 0) {
            Popup.show(requireContext(), "Command failed. Please check output for the details.");
        }
        inUri = null;
    }

    private void runTranscode() {
        clearLog();

        Log.d(MainActivity.TAG, "Testing transcode(" + inUri + ", " + outUri + ")");

        int result = Config.runTranscode(Config.getCommandParameter(getContext(), inUri), Config.getCommandParameter(getContext(), outUri));
        Log.d(MainActivity.TAG, String.format("Transcode exited with rc %d", result));

        if (result != 0) {
            Popup.show(requireContext(), "Command failed. Please check output for the details.");
        }

        inUri = outUri;
        outUri = null;
        if (result == 0) {
            runFFprobe();
        }
    }

    private void setActive() {
        Log.i(MainActivity.TAG, "ScopedStorage Tab Activated");
        enableLogCallback();
    }

    public void appendLog(final String logMessage) {
        outputText.append(logMessage);
    }

    public void clearLog() {
        outputText.setText("");
    }

    @Override
    public void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_SAF_FFPROBE && resultCode == RESULT_OK && data != null) {
            inUri = data.getData();
            runFFprobe();
        } else if (requestCode == REQUEST_SAF_TRANSCODE_IN && resultCode == RESULT_OK && data != null) {
            inUri = data.getData();
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT)
                    .setType("audio/*")
                    .putExtra(Intent.EXTRA_TITLE, "transcode.aac")
                    .addCategory(Intent.CATEGORY_OPENABLE);
            startActivityForResult(intent, REQUEST_SAF_TRANSCODE_OUT);
        } else if (requestCode == REQUEST_SAF_TRANSCODE_OUT && resultCode == RESULT_OK && data != null) {
            outUri = data.getData();
            runTranscode();
        } else {
            super.onActivityResult(requestCode, resultCode, data);
        }
    }
}
