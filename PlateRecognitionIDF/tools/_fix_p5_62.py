# -*- coding: utf-8 -*-
import io, sys
P = r"G:\All_Project\AI_Project\Smart-Paring-Iot\PlateRecognitionIDF\main\main.cpp"
src = io.open(P, encoding="utf-8", newline="").read()
A = u'ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 上下[%.0f%%,%.0f%%]%s | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",'
B = u'ESP_LOGI(TAG, "#%d ROI %dx%d 占屏%.1f%%%s 上下[%.0f%%,%.0f%%] | 定位%lld 预处理%lld 推理%lld ms | 平均%dx%d 归一x%.2f | B%d G%d R%d",'
if src.count(A) != 1:
    print("A count=%d ABORT" % src.count(A)); sys.exit(1)
src = src.replace(A, B, 1)
C = u"                     (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);\n                     (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);\n"
D = u"                     (g_pad_u_l - ROI_TRIM_X) * 100.0f, (g_pad_u_r - ROI_TRIM_X) * 100.0f);\n"
if src.count(C) != 1:
    print("C count=%d ABORT" % src.count(C)); sys.exit(1)
src = src.replace(C, D, 1)
io.open(P, "w", encoding="utf-8", newline="").write(src)
print("OK fixed 2 spots")
