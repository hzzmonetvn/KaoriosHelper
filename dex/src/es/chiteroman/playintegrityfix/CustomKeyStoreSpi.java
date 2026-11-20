/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * Copyright (C) 2026 HzzMonet / Kaorios Project
 * Adapted from osm0sis/PlayIntegrityFork (GPL-3.0-only) @ 23ec56c2440c4fd1a5e893a23ef7861d6366a828
 */

package es.chiteroman.playintegrityfix;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.security.Key;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.NoSuchAlgorithmException;
import java.security.UnrecoverableEntryException;
import java.security.UnrecoverableKeyException;
import java.security.cert.Certificate;
import java.security.cert.CertificateException;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.Locale;

/**
 * Delegating KeyStoreSpi that blocks DroidGuard hardware key attestation requests
 * when TEE hardware attestation simulation is inactive and PIF spoofProvider=1.
 */
public final class CustomKeyStoreSpi extends KeyStoreSpi {
    private final KeyStoreSpi delegate;

    public CustomKeyStoreSpi() {
        this.delegate = CustomProvider.newOriginalKeyStoreSpi();
    }

    public CustomKeyStoreSpi(KeyStoreSpi delegate) {
        this.delegate = delegate;
    }

    private KeyStoreSpi requireDelegate() throws KeyStoreException {
        if (delegate == null) {
            throw new KeyStoreException("Original AndroidKeyStore KeyStoreSpi unavailable");
        }
        return delegate;
    }

    static boolean isDroidGuardCaller(StackTraceElement[] frames) {
        if (frames == null) return false;
        for (int i = 0; i < frames.length; i++) {
            String clsName = frames[i].getClassName();
            if (clsName != null && clsName.toLowerCase(Locale.ROOT).contains("droidguard")) {
                return true;
            }
        }
        return false;
    }

    @Override
    public Certificate[] engineGetCertificateChain(String alias) {
        if (isDroidGuardCaller(Thread.currentThread().getStackTrace())) {
            CustomProvider.recordIntercept();
            throw new UnsupportedOperationException("DroidGuard KeyStore attestation blocked by PIF CustomProvider");
        }
        if (delegate == null) {
            return null;
        }
        return delegate.engineGetCertificateChain(alias);
    }

    @Override
    public Key engineGetKey(String alias, char[] password)
            throws NoSuchAlgorithmException, UnrecoverableKeyException {
        if (delegate == null) {
            throw new UnrecoverableKeyException("Original KeyStoreSpi unavailable");
        }
        return delegate.engineGetKey(alias, password);
    }

    @Override
    public Certificate engineGetCertificate(String alias) {
        return delegate != null ? delegate.engineGetCertificate(alias) : null;
    }

    @Override
    public Date engineGetCreationDate(String alias) {
        return delegate != null ? delegate.engineGetCreationDate(alias) : null;
    }

    @Override
    public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain)
            throws KeyStoreException {
        requireDelegate().engineSetKeyEntry(alias, key, password, chain);
    }

    @Override
    public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain)
            throws KeyStoreException {
        requireDelegate().engineSetKeyEntry(alias, key, chain);
    }

    @Override
    public void engineSetCertificateEntry(String alias, Certificate cert)
            throws KeyStoreException {
        requireDelegate().engineSetCertificateEntry(alias, cert);
    }

    @Override
    public void engineDeleteEntry(String alias) throws KeyStoreException {
        requireDelegate().engineDeleteEntry(alias);
    }

    @Override
    public Enumeration<String> engineAliases() {
        return delegate != null ? delegate.engineAliases() : Collections.emptyEnumeration();
    }

    @Override
    public boolean engineContainsAlias(String alias) {
        return delegate != null && delegate.engineContainsAlias(alias);
    }

    @Override
    public int engineSize() {
        return delegate != null ? delegate.engineSize() : 0;
    }

    @Override
    public boolean engineIsKeyEntry(String alias) {
        return delegate != null && delegate.engineIsKeyEntry(alias);
    }

    @Override
    public boolean engineIsCertificateEntry(String alias) {
        return delegate != null && delegate.engineIsCertificateEntry(alias);
    }

    @Override
    public String engineGetCertificateAlias(Certificate cert) {
        return delegate != null ? delegate.engineGetCertificateAlias(cert) : null;
    }

    @Override
    public void engineStore(OutputStream stream, char[] password)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (delegate != null) {
            delegate.engineStore(stream, password);
        }
    }

    @Override
    public void engineStore(KeyStore.LoadStoreParameter param)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (delegate != null) {
            delegate.engineStore(param);
        }
    }

    @Override
    public void engineLoad(InputStream stream, char[] password)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (delegate != null) {
            delegate.engineLoad(stream, password);
        }
    }

    @Override
    public void engineLoad(KeyStore.LoadStoreParameter param)
            throws IOException, NoSuchAlgorithmException, CertificateException {
        if (delegate != null) {
            delegate.engineLoad(param);
        }
    }

    @Override
    public KeyStore.Entry engineGetEntry(String alias, KeyStore.ProtectionParameter protParam)
            throws KeyStoreException, NoSuchAlgorithmException, UnrecoverableEntryException {
        return requireDelegate().engineGetEntry(alias, protParam);
    }

    @Override
    public void engineSetEntry(String alias, KeyStore.Entry entry, KeyStore.ProtectionParameter protParam)
            throws KeyStoreException {
        requireDelegate().engineSetEntry(alias, entry, protParam);
    }

    @Override
    public boolean engineEntryInstanceOf(String alias, Class<? extends KeyStore.Entry> entryClass) {
        return delegate != null && delegate.engineEntryInstanceOf(alias, entryClass);
    }
}
