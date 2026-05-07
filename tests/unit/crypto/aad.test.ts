// Tests for `lib/crypto/aad.ts`.

import { describe, it, expect } from 'vitest';

import { aadForItemToken, aadForPccDekWrap, aadForUserDekWrap } from '../../../lib/crypto/aad.js';
import { ItemId, UserId } from '../../../lib/types/domain.js';

const utf8 = (b: Uint8Array): string => Buffer.from(b).toString('utf8');

describe('aadForItemToken', () => {
  it('matches the locked personal AAD format', () => {
    const aad = aadForItemToken({
      domain: 'personal',
      itemId: ItemId('item_abc'),
      keyVersion: 3,
    });
    expect(utf8(aad)).toBe('personal:itemtoken:item_abc:3');
  });

  it('matches the locked PCC AAD format', () => {
    const aad = aadForItemToken({
      domain: 'pcc',
      itemId: ItemId('item_xyz'),
      keyVersion: 7,
    });
    expect(utf8(aad)).toBe('pcc:itemtoken:item_xyz:7');
  });

  it('rejects empty itemId', () => {
    expect(() =>
      aadForItemToken({ domain: 'personal', itemId: ItemId(''), keyVersion: 1 }),
    ).toThrow(/itemId must be non-empty/);
  });

  it('rejects itemId containing colon (delimiter collision)', () => {
    expect(() =>
      aadForItemToken({ domain: 'pcc', itemId: ItemId('a:b'), keyVersion: 1 }),
    ).toThrow(/must not contain ':'/);
  });

  it('rejects non-positive keyVersion', () => {
    expect(() =>
      aadForItemToken({ domain: 'pcc', itemId: ItemId('x'), keyVersion: 0 }),
    ).toThrow(/keyVersion/);
    expect(() =>
      aadForItemToken({ domain: 'pcc', itemId: ItemId('x'), keyVersion: -1 }),
    ).toThrow(/keyVersion/);
  });

  it('rejects non-integer keyVersion', () => {
    expect(() =>
      aadForItemToken({ domain: 'pcc', itemId: ItemId('x'), keyVersion: 1.5 }),
    ).toThrow(/keyVersion/);
  });

  it('produces different bytes for personal vs pcc domain', () => {
    const a = aadForItemToken({ domain: 'personal', itemId: ItemId('id'), keyVersion: 1 });
    const b = aadForItemToken({ domain: 'pcc', itemId: ItemId('id'), keyVersion: 1 });
    expect(utf8(a)).not.toBe(utf8(b));
  });

  it('produces different bytes for different keyVersion (rotation invalidates old AAD)', () => {
    const a = aadForItemToken({ domain: 'pcc', itemId: ItemId('id'), keyVersion: 1 });
    const b = aadForItemToken({ domain: 'pcc', itemId: ItemId('id'), keyVersion: 2 });
    expect(utf8(a)).not.toBe(utf8(b));
  });
});

describe('aadForUserDekWrap', () => {
  it('matches the locked personal:userdek format', () => {
    const aad = aadForUserDekWrap({ userId: UserId('usr_rory') });
    expect(utf8(aad)).toBe('personal:userdek:usr_rory');
  });

  it('rejects empty userId', () => {
    expect(() => aadForUserDekWrap({ userId: UserId('') })).toThrow(/userId/);
  });
});

describe('aadForPccDekWrap', () => {
  it('matches the locked pcc:dekwrap:v<n> format', () => {
    const aad = aadForPccDekWrap({ version: 5 });
    expect(utf8(aad)).toBe('pcc:dekwrap:v5');
  });

  it('rejects non-positive version', () => {
    expect(() => aadForPccDekWrap({ version: 0 })).toThrow(/version/);
    expect(() => aadForPccDekWrap({ version: -1 })).toThrow(/version/);
    expect(() => aadForPccDekWrap({ version: 1.5 })).toThrow(/version/);
  });
});
