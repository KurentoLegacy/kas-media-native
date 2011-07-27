package com.tikal.android.media.tx;

import com.tikal.android.media.profiles.VideoProfile;

/**
 * 
 * @author Miguel París Díaz
 * 
 */
public class VideoInfoTx {

	private VideoProfile videoProfile;
	private int payloadType;
	private String out;

	public VideoProfile getVideoProfile() {
		return videoProfile;
	}

	public void setVideoProfile(VideoProfile videoProfile) {
		this.videoProfile = videoProfile;
	}

	public int getPayloadType() {
		return payloadType;
	}

	public void setPayloadType(int payloadType) {
		this.payloadType = payloadType;
	}

	public String getOut() {
		return out;
	}

	public void setOut(String out) {
		this.out = out;
	}

	public VideoInfoTx(VideoProfile videoProfile) {
		this.videoProfile = videoProfile;
	}

}
