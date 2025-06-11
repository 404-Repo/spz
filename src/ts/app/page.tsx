'use client';

import { useState, useEffect, useRef, useCallback } from 'react';
import Script from 'next/script';
import styles from './page.module.css';

// eslint-disable-next-line @typescript-eslint/no-explicit-any
declare const Module: any;

// eslint-disable-next-line @typescript-eslint/no-explicit-any
declare const zip: any;

// Extend window type for Module
declare global {
  interface Window {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    Module: any;
  }
}

class MemoryManager {
  inputPtr: number | null = null;
  outputPtrPtr: number | null = null;
  outputSizePtr: number | null = null;
  outputPtr: number | null = null;

  prepareMemory(inputData: Uint8Array) {
    const inputSize = inputData.length;
    this.inputPtr = Module._malloc(inputSize);
    Module.HEAPU8.set(inputData, this.inputPtr);
    this.outputPtrPtr = Module._malloc(4);
    this.outputSizePtr = Module._malloc(4);
  }

  getOutputPtr() {
    this.outputPtr = Module.getValue(this.outputPtrPtr, 'i32');
    return this.outputPtr;
  }

  getOutputSize() {
    return Module.getValue(this.outputSizePtr, 'i32');
  }

  freeAll() {
    if (this.inputPtr !== null) Module._free(this.inputPtr);
    if (this.outputPtrPtr !== null) Module._free(this.outputPtrPtr);
    if (this.outputSizePtr !== null) Module._free(this.outputSizePtr);
    if (this.outputPtr !== null) Module._free(this.outputPtr);
    this.inputPtr = null;
    this.outputPtrPtr = null;
    this.outputSizePtr = null;
    this.outputPtr = null;
  }
}

export default function Home() {
  const [moduleReady, setModuleReady] = useState(false);
  const [compressFiles, setCompressFiles] = useState<File[]>([]);
  const [decompressFiles, setDecompressFiles] = useState<File[]>([]);
  const [compressStatus, setCompressStatus] = useState('');
  const [decompressStatus, setDecompressStatus] = useState('');
  const [includeNormals, setIncludeNormals] = useState(false);
  const [compressResults, setCompressResults] = useState<{ name: string; blob: Blob }[]>([]);
  const [decompressResults, setDecompressResults] = useState<{ name: string; blob: Blob }[]>([]);
  const compressDropZoneRef = useRef<HTMLDivElement>(null);
  const decompressDropZoneRef = useRef<HTMLDivElement>(null);
  const compressFileInputRef = useRef<HTMLInputElement>(null);
  const decompressFileInputRef = useRef<HTMLInputElement>(null);

  const processCompressFiles = useCallback(async (files: File[]) => {
    setCompressStatus('Processing...');
    const memory = new MemoryManager();
    const results: { name: string; blob: Blob }[] = [];
    let success = 0;
    let errors = 0;

    for (const file of files) {
      try {
        const data = new Uint8Array(await file.arrayBuffer());
        memory.prepareMemory(data);
        const retCode = Module._compress_spz(memory.inputPtr, data.length, 3, memory.outputPtrPtr, memory.outputSizePtr);
        if (retCode === 0) {
          const outputPtr = memory.getOutputPtr();
          const outputSize = memory.getOutputSize();
          if (outputPtr !== null && outputSize > 0) {
            const outputData = new Uint8Array(Module.HEAPU8.buffer, outputPtr, outputSize);
            results.push({ name: file.name.replace(/\.[^/.]+$/, '') + '.spz', blob: new Blob([outputData]) });
            success++;
          } else {
            errors++;
          }
        } else {
          errors++;
        }
      } catch {
        errors++;
      } finally {
        memory.freeAll();
      }
    }

    setCompressResults(results);
    setCompressStatus(`Success: ${success}, Errors: ${errors}`);
  }, []);

  const processDecompressFiles = useCallback(async (files: File[]) => {
    setDecompressStatus('Processing...');
    const memory = new MemoryManager();
    const results: { name: string; blob: Blob }[] = [];
    let success = 0;
    let errors = 0;

    for (const file of files) {
      try {
        const data = new Uint8Array(await file.arrayBuffer());
        memory.prepareMemory(data);
        const retCode = Module._decompress_spz(memory.inputPtr, data.length, includeNormals ? 1 : 0, memory.outputPtrPtr, memory.outputSizePtr);
        if (retCode === 0) {
          const outputPtr = memory.getOutputPtr();
          const outputSize = memory.getOutputSize();
          if (outputPtr !== null && outputSize > 0) {
            const outputData = new Uint8Array(Module.HEAPU8.buffer, outputPtr, outputSize);
            let name = file.name.replace(/\.spz$/i, '');
            if (!name.toLowerCase().endsWith('.ply')) name += '.ply';
            results.push({ name, blob: new Blob([outputData]) });
            success++;
          } else {
            errors++;
          }
        } else {
          errors++;
        }
      } catch {
        errors++;
      } finally {
        memory.freeAll();
      }
    }

    setDecompressResults(results);
    setDecompressStatus(`Success: ${success}, Errors: ${errors}`);
  }, [includeNormals]);

  useEffect(() => {
    const setupDropZone = (
      ref: React.RefObject<HTMLDivElement | null>,
      setFiles: (files: File[]) => void,
      process: (files: File[]) => void,
      fileInputRef: React.RefObject<HTMLInputElement | null>
    ) => {
      const zone = ref.current;
      if (!zone) return;

      ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(event => {
        zone.addEventListener(event, (e) => e.preventDefault());
      });

      zone.addEventListener('drop', (e) => {
        const files = Array.from(e.dataTransfer?.files || []);
        setFiles(files);
        process(files);
      });

      // Add click handler to trigger file input
      zone.addEventListener('click', () => {
        fileInputRef.current?.click();
      });
    };

    setupDropZone(compressDropZoneRef, setCompressFiles, processCompressFiles, compressFileInputRef);
    setupDropZone(decompressDropZoneRef, setDecompressFiles, processDecompressFiles, decompressFileInputRef);
  }, [processCompressFiles, processDecompressFiles]);

  const downloadAll = async (results: { name: string; blob: Blob }[], filename: string) => {
    if (results.length === 0) return;
    const zipWriter = new zip.ZipWriter(new zip.BlobWriter('application/zip'));
    for (const { name, blob } of results) {
      await zipWriter.add(name, new zip.BlobReader(blob), { level: 0 });
    }
    const blob = await zipWriter.close();
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  };

  return (
    <div className={styles.container}>
      <header className={styles.header}>
        <h1 className={styles.title}>SPZ File Processor</h1>
        <p className={styles.subtitle}>Compress PLY and decompress SPZ files with ease</p>
      </header>

      <main className={styles.main}>
        <section className={styles.section}>
          <div className={styles.sectionHeader}>
            <h2 className={styles.sectionTitle}>Compression</h2>
            {!moduleReady && (
              <div className={styles.statusBadge}>
                <span className={styles.statusLoading}>Loading...</span>
              </div>
            )}
          </div>

          <div className={styles.inputGroup}>
            <input
              ref={compressFileInputRef}
              type="file"
              multiple
              onChange={(e) => setCompressFiles(Array.from(e.target.files || []))}
              className={styles.fileInput}
              id="compress-files"
            />
            <label htmlFor="compress-files" className={styles.fileInputLabel}>
              Choose Files to Compress
            </label>
          </div>

          <div
            ref={compressDropZoneRef}
            className={`${styles.dropZone} ${compressFiles.length > 0 ? styles.dropZoneActive : ''}`}
          >
            <div className={styles.dropZoneContent}>
              <svg className={styles.dropZoneIcon} fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M7 16a4 4 0 01-.88-7.903A5 5 0 1115.9 6L16 6a5 5 0 011 9.9M15 13l-3-3m0 0l-3 3m3-3v12" />
              </svg>
              <p>Drop files here to compress</p>
              <span className={styles.dropZoneHint}>or click to browse</span>
            </div>
          </div>

          <div className={styles.controlsGroup}>
            { }
            <div className={styles.checkboxGroup}>
            </div>

            <div className={styles.controls}>
              <button
                onClick={() => processCompressFiles(compressFiles)}
                disabled={!moduleReady || compressFiles.length === 0}
                className={`${styles.button} ${styles.buttonPrimary}`}
              >
                Compress Files ({compressFiles.length})
              </button>
            </div>
          </div>

          {compressStatus && (
            <div className={styles.statusMessage}>
              {compressStatus}
            </div>
          )}

          {compressResults.length > 0 && (
            <div className={styles.results}>
              <h3 className={styles.resultsTitle}>Compressed Files</h3>
              <div className={styles.fileList}>
                {compressResults.map((r, i) => (
                  <div key={i} className={styles.fileItem}>
                    <span className={styles.fileName}>{r.name}</span>
                    <a
                      href={URL.createObjectURL(r.blob)}
                      download={r.name}
                      className={styles.downloadLink}
                    >
                      Download
                    </a>
                  </div>
                ))}
              </div>
              <button
                onClick={() => downloadAll(compressResults, 'compressed_files.zip')}
                className={`${styles.button} ${styles.buttonSecondary}`}
              >
                Download All as ZIP
              </button>
            </div>
          )}
        </section>

        <section className={styles.section}>
          <div className={styles.sectionHeader}>
            <h2 className={styles.sectionTitle}>Decompression</h2>
            {!moduleReady && (
              <div className={styles.statusBadge}>
                <span className={styles.statusLoading}>Loading...</span>
              </div>
            )}
          </div>

          <div className={styles.inputGroup}>
            <input
              ref={decompressFileInputRef}
              type="file"
              multiple
              onChange={(e) => setDecompressFiles(Array.from(e.target.files || []))}
              className={styles.fileInput}
              id="decompress-files"
            />
            <label htmlFor="decompress-files" className={styles.fileInputLabel}>
              Choose SPZ Files to Decompress
            </label>
          </div>

          <div
            ref={decompressDropZoneRef}
            className={`${styles.dropZone} ${decompressFiles.length > 0 ? styles.dropZoneActive : ''}`}
          >
            <div className={styles.dropZoneContent}>
              <svg className={styles.dropZoneIcon} fill="none" stroke="currentColor" viewBox="0 0 24 24">
                <path strokeLinecap="round" strokeLinejoin="round" strokeWidth={2} d="M7 16a4 4 0 01-.88-7.903A5 5 0 1115.9 6L16 6a5 5 0 011 9.9M9 12l2 2 4-4" />
              </svg>
              <p>Drop SPZ files here to decompress</p>
              <span className={styles.dropZoneHint}>or click to browse</span>
            </div>
          </div>

          <div className={styles.controlsGroup}>
            <div className={styles.checkboxGroup}>
              <label className={styles.checkboxLabel}>
                <input
                  type="checkbox"
                  checked={includeNormals}
                  onChange={(e) => setIncludeNormals(e.target.checked)}
                  className={styles.checkbox}
                />
                <span className={styles.checkboxText}>Include Normals</span>
              </label>
            </div>

            <div className={styles.controls}>
              <button
                onClick={() => processDecompressFiles(decompressFiles)}
                disabled={!moduleReady || decompressFiles.length === 0}
                className={`${styles.button} ${styles.buttonPrimary}`}
              >
                Decompress Files ({decompressFiles.length})
              </button>
            </div>
          </div>

          {decompressStatus && (
            <div className={styles.statusMessage}>
              {decompressStatus}
            </div>
          )}

          {decompressResults.length > 0 && (
            <div className={styles.results}>
              <h3 className={styles.resultsTitle}>Decompressed Files</h3>
              <div className={styles.fileList}>
                {decompressResults.map((r, i) => (
                  <div key={i} className={styles.fileItem}>
                    <span className={styles.fileName}>{r.name}</span>
                    <a
                      href={URL.createObjectURL(r.blob)}
                      download={r.name}
                      className={styles.downloadLink}
                    >
                      Download
                    </a>
                  </div>
                ))}
              </div>
              <button
                onClick={() => downloadAll(decompressResults, 'decompressed_files.zip')}
                className={`${styles.button} ${styles.buttonSecondary}`}
              >
                Download All as ZIP
              </button>
            </div>
          )}
        </section>
      </main>

      <Script
        src="/spz_wasm.js"
        strategy="afterInteractive"
        onLoad={() => {
          window.Module.onRuntimeInitialized = () => setModuleReady(true);
        }}
      />
      <Script src="https://unpkg.com/@zip.js/zip.js@2.7.62/dist/zip.min.js" strategy="afterInteractive" />
    </div>
  );
}
