// Serves the app's files directory as content:// URIs, as a document
// provider (a USB drive's, the Storage Access Framework's) would:
//   content://tech.soit.lychee.example.media/NAME       a file descriptor
//   content://tech.soit.lychee.example.media/pipe/NAME  a pipe, as
//                                                       providers that
//                                                       stream give
// tool/android_check.sh copies media there with run-as.

package tech.soit.lychee.example

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.net.Uri
import android.os.ParcelFileDescriptor
import java.io.File
import java.io.FileNotFoundException
import java.io.IOException

class MediaProvider : ContentProvider() {
    override fun onCreate() = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor {
        val segments = uri.pathSegments
        val pipe = segments.size == 2 && segments[0] == "pipe"
        val name = segments.lastOrNull() ?: throw FileNotFoundException("$uri")
        val file = File(context!!.filesDir, name)
        if (name.contains('/') || !file.isFile) throw FileNotFoundException("$uri")
        if (!pipe) return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY)
        val (read, write) = ParcelFileDescriptor.createPipe()
        Thread {
            try {
                ParcelFileDescriptor.AutoCloseOutputStream(write).use { out ->
                    file.inputStream().use { it.copyTo(out) }
                }
            } catch (_: IOException) {
                // The reader closed its end.
            }
        }.start()
        return read
    }

    override fun getType(uri: Uri): String? = null
    override fun query(
        uri: Uri, projection: Array<out String>?, selection: String?,
        selectionArgs: Array<out String>?, sortOrder: String?,
    ): Cursor? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
    override fun update(
        uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?,
    ) = 0
}
