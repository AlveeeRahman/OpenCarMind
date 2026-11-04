---
name: Model architecture
about: Propose a network for the on-board model. We train it on our data and post the scores.
title: "Architecture: "
labels: model
---

**The idea in one or two sentences, and why you expect it to beat CarMind GenEdge**

**The network**
Attach a PyTorch `nn.Module` (a single file) or an ONNX graph, or link to one.

**Checklist** (see docs/MODEL_CASE_STUDY.md, "Send us an architecture")
- [ ] Input `[1, 30, 1, W]` (10 values, 10 masks, 10 differences per one-second step, oldest first). W you want: 
- [ ] Outputs: 4 manoeuvre logits and 3 standardised estimates
- [ ] Uses only operations ESP-DL runs (convolution, depthwise convolution, ReLU, Add, ReduceMean, Concat and Gemm are known to work)
- [ ] Expected size as int8 under about 150 KB
- [ ] Parameter count: 

**How to credit you in the case study** (name or handle, link)
