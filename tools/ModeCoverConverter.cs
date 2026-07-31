using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

public static class ModeCoverConverter
{
    private const int Width = 800;
    private const int Height = 480;
    private const int PixelCount = Width * Height;
    private const int FrameBytes = PixelCount / 2;

    // Same palette order and panel codes as CandidateSixColorProfile in the Android App.
    private static readonly int[,] Palette = new int[,]
    {
        { 0,   0,   0 },
        { 255, 255, 255 },
        { 255, 0,   0 },
        { 0,   255, 0 },
        { 0,   0,   255 },
        { 255, 255, 0 },
    };

    private static readonly byte[] PanelCodes = { 0, 1, 3, 6, 5, 2 };

    public static void Convert(
        string inputPath,
        string outputDirectory,
        string assetId,
        string ownerFeature,
        string displayName)
    {
        Directory.CreateDirectory(outputDirectory);

        using (var source = new Bitmap(inputPath))
        using (var composed = ResizeToPanel(source))
        {
            int[] workRgb = ReadRgb(composed);
            byte[] paletteIndices = new byte[PixelCount];
            byte[] previewRgb = new byte[PixelCount * 3];

            for (int offset = 0; offset < PixelCount; offset++)
            {
                int baseOffset = offset * 3;
                int red = workRgb[baseOffset];
                int green = workRgb[baseOffset + 1];
                int blue = workRgb[baseOffset + 2];
                int paletteIndex = NearestPaletteIndex(red, green, blue);

                paletteIndices[offset] = (byte)paletteIndex;
                previewRgb[baseOffset] = (byte)Palette[paletteIndex, 0];
                previewRgb[baseOffset + 1] = (byte)Palette[paletteIndex, 1];
                previewRgb[baseOffset + 2] = (byte)Palette[paletteIndex, 2];

                DiffuseFloydSteinberg(
                    workRgb,
                    offset % Width,
                    offset / Width,
                    red - Palette[paletteIndex, 0],
                    green - Palette[paletteIndex, 1],
                    blue - Palette[paletteIndex, 2]);
            }

            byte[] frame = Pack4Bpp(paletteIndices);
            ValidateFrame(frame);

            string previewPath = Path.Combine(outputDirectory, assetId + "_preview.png");
            string framePath = Path.Combine(outputDirectory, assetId + ".bin");
            string manifestPath = Path.Combine(outputDirectory, assetId + ".manifest.json");

            WritePreview(previewRgb, previewPath);
            File.WriteAllBytes(framePath, frame);
            string sha256 = Sha256Hex(frame);
            WriteManifest(manifestPath, assetId, ownerFeature, displayName, sha256);

            Console.WriteLine(assetId + ": preview=" + previewPath);
            Console.WriteLine(assetId + ": frame=" + framePath + " bytes=" + frame.Length + " sha256=" + sha256);
            Console.WriteLine(assetId + ": manifest=" + manifestPath);
        }
    }

    private static Bitmap ResizeToPanel(Bitmap source)
    {
        var output = new Bitmap(Width, Height, PixelFormat.Format24bppRgb);
        using (Graphics graphics = Graphics.FromImage(output))
        {
            graphics.Clear(Color.White);
            graphics.CompositingMode = CompositingMode.SourceCopy;
            graphics.CompositingQuality = CompositingQuality.HighQuality;
            graphics.InterpolationMode = InterpolationMode.HighQualityBicubic;
            graphics.PixelOffsetMode = PixelOffsetMode.HighQuality;
            graphics.SmoothingMode = SmoothingMode.HighQuality;
            graphics.DrawImage(source, new Rectangle(0, 0, Width, Height));
        }
        return output;
    }

    private static int[] ReadRgb(Bitmap bitmap)
    {
        var result = new int[PixelCount * 3];
        Rectangle rect = new Rectangle(0, 0, Width, Height);
        BitmapData data = bitmap.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format24bppRgb);
        try
        {
            int stride = Math.Abs(data.Stride);
            byte[] buffer = new byte[stride * Height];
            Marshal.Copy(data.Scan0, buffer, 0, buffer.Length);
            for (int y = 0; y < Height; y++)
            {
                int row = data.Stride >= 0 ? y * stride : (Height - 1 - y) * stride;
                for (int x = 0; x < Width; x++)
                {
                    int src = row + x * 3;
                    int dst = (y * Width + x) * 3;
                    result[dst] = buffer[src + 2];
                    result[dst + 1] = buffer[src + 1];
                    result[dst + 2] = buffer[src];
                }
            }
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
        return result;
    }

    private static int NearestPaletteIndex(int red, int green, int blue)
    {
        int bestIndex = 0;
        int bestDistance = 999999;
        for (int index = 0; index < Palette.GetLength(0); index++)
        {
            int dr = red - Palette[index, 0];
            int dg = green - Palette[index, 1];
            int db = blue - Palette[index, 2];
            int distance = dr * dr + dg * dg + db * db;
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = index;
            }
        }
        return bestIndex;
    }

    private static void DiffuseFloydSteinberg(
        int[] workRgb,
        int x,
        int y,
        int redError,
        int greenError,
        int blueError)
    {
        if (x + 1 < Width)
        {
            AddError(workRgb, y * Width + x + 1, 7, redError, greenError, blueError);
        }
        if (y + 1 < Height)
        {
            if (x > 0)
            {
                AddError(workRgb, (y + 1) * Width + x - 1, 3, redError, greenError, blueError);
            }
            AddError(workRgb, (y + 1) * Width + x, 5, redError, greenError, blueError);
            if (x + 1 < Width)
            {
                AddError(workRgb, (y + 1) * Width + x + 1, 1, redError, greenError, blueError);
            }
        }
    }

    private static void AddError(
        int[] workRgb,
        int pixelOffset,
        int weight,
        int redError,
        int greenError,
        int blueError)
    {
        int baseOffset = pixelOffset * 3;
        workRgb[baseOffset] = Clamp(workRgb[baseOffset] + redError * weight / 16);
        workRgb[baseOffset + 1] = Clamp(workRgb[baseOffset + 1] + greenError * weight / 16);
        workRgb[baseOffset + 2] = Clamp(workRgb[baseOffset + 2] + blueError * weight / 16);
    }

    private static int Clamp(int value)
    {
        return value < 0 ? 0 : (value > 255 ? 255 : value);
    }

    private static byte[] Pack4Bpp(byte[] paletteIndices)
    {
        var output = new byte[FrameBytes];
        for (int byteOffset = 0; byteOffset < FrameBytes; byteOffset++)
        {
            int firstIndex = paletteIndices[byteOffset * 2];
            int secondIndex = paletteIndices[byteOffset * 2 + 1];
            output[byteOffset] = (byte)((PanelCodes[firstIndex] << 4) | PanelCodes[secondIndex]);
        }
        return output;
    }

    private static void ValidateFrame(byte[] frame)
    {
        if (frame.Length != FrameBytes)
        {
            throw new InvalidDataException("Mode cover frame length mismatch");
        }
        for (int offset = 0; offset < frame.Length; offset++)
        {
            int high = frame[offset] >> 4;
            int low = frame[offset] & 0x0F;
            if (!IsPanelCode(high) || !IsPanelCode(low))
            {
                throw new InvalidDataException("Unknown panel code at byte " + offset);
            }
        }
    }

    private static bool IsPanelCode(int value)
    {
        for (int i = 0; i < PanelCodes.Length; i++)
        {
            if (PanelCodes[i] == value) return true;
        }
        return false;
    }

    private static void WritePreview(byte[] rgb, string path)
    {
        using (var bitmap = new Bitmap(Width, Height, PixelFormat.Format24bppRgb))
        {
            Rectangle rect = new Rectangle(0, 0, Width, Height);
            BitmapData data = bitmap.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format24bppRgb);
            try
            {
                int stride = Math.Abs(data.Stride);
                byte[] buffer = new byte[stride * Height];
                for (int y = 0; y < Height; y++)
                {
                    int row = data.Stride >= 0 ? y * stride : (Height - 1 - y) * stride;
                    for (int x = 0; x < Width; x++)
                    {
                        int src = (y * Width + x) * 3;
                        int dst = row + x * 3;
                        buffer[dst] = rgb[src + 2];
                        buffer[dst + 1] = rgb[src + 1];
                        buffer[dst + 2] = rgb[src];
                    }
                }
                Marshal.Copy(buffer, 0, data.Scan0, buffer.Length);
            }
            finally
            {
                bitmap.UnlockBits(data);
            }
            bitmap.Save(path, ImageFormat.Png);
        }
    }

    private static string Sha256Hex(byte[] data)
    {
        using (SHA256 sha256 = SHA256.Create())
        {
            byte[] digest = sha256.ComputeHash(data);
            var builder = new StringBuilder(digest.Length * 2);
            foreach (byte value in digest) builder.Append(value.ToString("x2"));
            return builder.ToString();
        }
    }

    private static void WriteManifest(
        string path,
        string assetId,
        string ownerFeature,
        string displayName,
        string sha256)
    {
        string json = "{\n" +
            "  \"manifest_version\": 1,\n" +
            "  \"system_asset_id\": \"" + EscapeJson(assetId) + "\",\n" +
            "  \"category\": \"system\",\n" +
            "  \"owner_feature\": \"" + EscapeJson(ownerFeature) + "\",\n" +
            "  \"display_name\": \"" + EscapeJson(displayName) + "\",\n" +
            "  \"display_profile\": {\n" +
            "    \"width\": 800,\n" +
            "    \"height\": 480,\n" +
            "    \"frame_bytes\": 192000,\n" +
            "    \"pixel_format\": \"4bpp\",\n" +
            "    \"palette\": \"six_color_e6\",\n" +
            "    \"rotation_degrees\": 0,\n" +
            "    \"converter_version\": \"official-photopainter-floyd-steinberg-rgb888-v1\"\n" +
            "  },\n" +
            "  \"image_bin\": {\n" +
            "    \"size_bytes\": 192000,\n" +
            "    \"sha256\": \"" + sha256 + "\"\n" +
            "  }\n" +
            "}\n";
        File.WriteAllText(path, json, new UTF8Encoding(false));
    }

    private static string EscapeJson(string value)
    {
        return value.Replace("\\", "\\\\").Replace("\"", "\\\"");
    }
}
