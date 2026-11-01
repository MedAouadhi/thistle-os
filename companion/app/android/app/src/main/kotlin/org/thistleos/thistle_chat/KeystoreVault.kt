package org.thistleos.thistle_chat

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * The identity's seed, sealed by an AES-256-GCM key that lives in the
 * Android Keystore and never leaves it (spec, The companion app:
 * Identity). The sealed seed is kept in the app's private preferences,
 * which nothing backs up (AndroidManifest.xml).
 */
object KeystoreVault {
    private const val ALIAS = "thistle_chat_identity"
    private const val PREFS = "identity"

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(ALIAS, null) as SecretKey?)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    /** The seed kept for `profile`, or null if none. Throws if it cannot be unsealed. */
    fun read(context: Context, profile: String): String? {
        val kept = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("seed.$profile", null)
            ?: return null
        val (iv, sealed) = kept.split(':').map { Base64.decode(it, Base64.NO_WRAP) }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(128, iv))
        return String(cipher.doFinal(sealed), Charsets.UTF_8)
    }

    fun write(context: Context, profile: String, seedHex: String) {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val sealed = cipher.doFinal(seedHex.toByteArray(Charsets.UTF_8))
        val kept = Base64.encodeToString(cipher.iv, Base64.NO_WRAP) + ":" +
            Base64.encodeToString(sealed, Base64.NO_WRAP)
        val ok = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putString("seed.$profile", kept).commit()
        if (!ok) throw IllegalStateException("the sealed identity could not be written")
    }
}
