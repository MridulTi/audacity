/**********************************************************************

  Audacity: A Digital Audio Editor

  DtmfGen.cpp

  Salvo Ventura - Dec 2006

*******************************************************************//**

\class EffectDtmf
\brief An effect that generates DTMF tones

*//*******************************************************************/


#include "DtmfGen.h"
#include "LoadEffects.h"

#include <wx/intl.h>
#include <wx/slider.h>
#include <wx/valgen.h>
#include <wx/valtext.h>
#include <wx/stattext.h>

#include "Prefs.h"
#include "../Shuttle.h"
#include "../ShuttleGui.h"
#include "../widgets/NumericTextCtrl.h"
#include "../widgets/valnum.h"


// Define keys, defaults, minimums, and maximums for the effect parameters
//
//     Name       Type        Key               Def                   Min      Max      Scale
Param( Sequence,  wxString,   wxT("Sequence"),   EffectDtmf::Settings::DefaultSequence,  wxT(""), wxT(""), wxT(""));
Param( DutyCycle, double,     wxT("Duty Cycle"), EffectDtmf::Settings::DefaultDutyCycle,   0.0,     100.0,   10.0   );
Param( Amplitude, double,     wxT("Amplitude"),  EffectDtmf::Settings::DefaultAmplitude,   0.001,   1.0,     1      );

static const double kFadeInOut = 250.0; // used for fadein/out needed to remove clicking noise

const static wxChar *kSymbols[] =
{
   wxT("0"), wxT("1"), wxT("2"), wxT("3"),
   wxT("4"), wxT("5"), wxT("6"), wxT("7"),
   wxT("8"), wxT("9"), wxT("*"), wxT("#"),
   wxT("A"), wxT("B"), wxT("C"), wxT("D"),
   wxT("a"), wxT("b"), wxT("c"), wxT("d"),
   wxT("e"), wxT("f"), wxT("g"), wxT("h"),
   wxT("i"), wxT("j"), wxT("k"), wxT("l"),
   wxT("m"), wxT("n"), wxT("o"), wxT("p"),
   wxT("q"), wxT("r"), wxT("s"), wxT("t"),
   wxT("u"), wxT("v"), wxT("w"), wxT("x"),
   wxT("y"), wxT("z")
};

//
// EffectDtmf
//

const ComponentInterfaceSymbol EffectDtmf::Symbol
{ XO("DTMF Tones") };

namespace{ BuiltinEffectsModule::Registration< EffectDtmf > reg; }

EffectDtmf::EffectDtmf()
{
}

EffectDtmf::~EffectDtmf()
{
}

// ComponentInterface implementation

ComponentInterfaceSymbol EffectDtmf::GetSymbol() const
{
   return Symbol;
}

TranslatableString EffectDtmf::GetDescription() const
{
   return XO("Generates dual-tone multi-frequency (DTMF) tones like those produced by the keypad on telephones");
}

ManualPageID EffectDtmf::ManualPage() const
{
   return L"DTMF_Tones";
}

// EffectDefinitionInterface implementation

EffectType EffectDtmf::GetType() const
{
   return EffectTypeGenerate;
}

// EffectProcessor implementation

unsigned EffectDtmf::GetAudioOutCount() const
{
   return 1;
}

bool EffectDtmf::ProcessInitialize(
   EffectSettings &, sampleCount, ChannelNames chanMap)
{
   auto &dtmfSettings = mSettings;
   if (dtmfSettings.dtmfNTones <= 0) {   // Bail if no DTFM sequence.
      ::Effect::MessageBox(
               XO("DTMF sequence empty.\nCheck ALL settings for this effect."),
         wxICON_ERROR );

      return false;
   }
   double duration = GetDuration();

   // all dtmf sequence durations in samples from seconds
   // MJS: Note that mDuration is in seconds but will have been quantised to the units of the TTC.
   // If this was 'samples' and the project rate was lower than the track rate,
   // extra samples may get created as mDuration may now be > mT1 - mT0;
   // However we are making our best efforts at creating what was asked for.

   auto nT0 = (sampleCount)floor(mT0 * mSampleRate + 0.5);
   auto nT1 = (sampleCount)floor((mT0 + duration) * mSampleRate + 0.5);
   numSamplesSequence = nT1 - nT0;  // needs to be exact number of samples selected

   //make under-estimates if anything, and then redistribute the few remaining samples
   numSamplesTone = sampleCount( floor(dtmfSettings.dtmfTone * mSampleRate) );
   numSamplesSilence = sampleCount( floor(dtmfSettings.dtmfSilence * mSampleRate) );

   // recalculate the sum, and spread the difference - due to approximations.
   // Since diff should be in the order of "some" samples, a division (resulting in zero)
   // is not sufficient, so we add the additional remaining samples in each tone/silence block,
   // at least until available.
   diff = numSamplesSequence - (dtmfSettings.dtmfNTones * numSamplesTone)
      - (dtmfSettings.dtmfNTones - 1) * numSamplesSilence;
   while (diff > 2 * dtmfSettings.dtmfNTones - 1) {   // more than one per thingToBeGenerated
      // in this case, both numSamplesTone and numSamplesSilence would change, so it makes sense
      //  to recalculate diff here, otherwise just keep the value we already have

      // should always be the case that dtmfNTones>1, as if 0, we don't even start processing,
      // and with 1 there is no difference to spread (no silence slot)...
      wxASSERT(dtmfSettings.dtmfNTones > 1);
      numSamplesTone += (diff / (dtmfSettings.dtmfNTones));
      numSamplesSilence += (diff / (dtmfSettings.dtmfNTones - 1));
      diff = numSamplesSequence - (dtmfSettings.dtmfNTones * numSamplesTone)
         - (dtmfSettings.dtmfNTones - 1) * numSamplesSilence;
   }
   wxASSERT(diff >= 0);  // should never be negative

   curSeqPos = -1; // pointer to string in dtmfSequence
   isTone = false;
   numRemaining = 0;

   return true;
}

size_t EffectDtmf::ProcessBlock(EffectSettings &,
   const float *const *, float *const *outbuf, size_t size)
{
   auto &dtmfSettings = mSettings;
   float *buffer = outbuf[0];
   decltype(size) processed = 0;

   // for the whole dtmf sequence, we will be generating either tone or silence
   // according to a bool value, and this might be done in small chunks of size
   // 'block', as a single tone might sometimes be larger than the block
   // tone and silence generally have different duration, thus two generation blocks
   //
   // Note: to overcome a 'clicking' noise introduced by the abrupt transition from/to
   // silence, I added a fade in/out of 1/250th of a second (4ms). This can still be
   // tweaked but gives excellent results at 44.1kHz: I haven't tried other freqs.
   // A problem might be if the tone duration is very short (<10ms)... (?)
   //
   // One more problem is to deal with the approximations done when calculating the duration
   // of both tone and silence: in some cases the final sum might not be same as the initial
   // duration. So, to overcome this, we had a redistribution block up, and now we will spread
   // the remaining samples in every bin in order to achieve the full duration: test case was
   // to generate an 11 tone DTMF sequence, in 4 seconds, and with DutyCycle=75%: after generation
   // you ended up with 3.999s or in other units: 3 seconds and 44097 samples.
   //
   while (size)
   {
      if (numRemaining == 0)
      {
         isTone = !isTone;

         if (isTone)
         {
            curSeqPos++;
            numRemaining = numSamplesTone;
            curTonePos = 0;
         }
         else
         {
            numRemaining = numSamplesSilence;
         }

         // the statement takes care of extracting one sample from the diff bin and
         // adding it into the current block until depletion
         numRemaining += (diff-- > 0 ? 1 : 0);         
      }

      const auto len = limitSampleBufferSize( size, numRemaining );

      if (isTone)
      {
         // generate the tone and append
         MakeDtmfTone(buffer, len, mSampleRate,
            dtmfSettings.dtmfSequence[curSeqPos], curTonePos, numSamplesTone,
            dtmfSettings.dtmfAmplitude);
         curTonePos += len;
      }
      else
      {
         memset(buffer, 0, sizeof(float) * len);
      }

      numRemaining -= len;

      buffer += len;
      size -= len;
      processed += len;
   }

   return processed;
}
bool EffectDtmf::VisitSettings( SettingsVisitor & S ){
   auto &dtmfSettings = mSettings;

   S.SHUTTLE_PARAM( dtmfSettings.dtmfSequence, Sequence );
   S.SHUTTLE_PARAM( dtmfSettings.dtmfDutyCycle, DutyCycle );
   S.SHUTTLE_PARAM( dtmfSettings.dtmfAmplitude, Amplitude );
   return true;
}

bool EffectDtmf::GetAutomationParameters(CommandParameters & parms) const
{
   auto &dtmfSettings = mSettings;

   parms.Write(KEY_Sequence, dtmfSettings.dtmfSequence);
   parms.Write(KEY_DutyCycle, dtmfSettings.dtmfDutyCycle);
   parms.Write(KEY_Amplitude, dtmfSettings.dtmfAmplitude);
   return true;
}

bool EffectDtmf::SetAutomationParameters(const CommandParameters & parms)
{
   auto &dtmfSettings = mSettings;

   ReadAndVerifyDouble(DutyCycle);
   ReadAndVerifyDouble(Amplitude);
   ReadAndVerifyString(Sequence);

   wxString symbols;
   for (unsigned int i = 0; i < WXSIZEOF(kSymbols); i++)
   {
      symbols += kSymbols[i];
   }

   if (Sequence.find_first_not_of(symbols) != wxString::npos)
   {
      return false;
   }

   dtmfSettings.dtmfDutyCycle = DutyCycle;
   dtmfSettings.dtmfAmplitude = Amplitude;
   dtmfSettings.dtmfSequence = Sequence;
   
   dtmfSettings.Recalculate(*this);

   return true;
}

namespace {
// Event handler object
struct MyValidator
   : DefaultEffectUIValidator
{
   MyValidator(EffectUIClientInterface &effect,
      EffectSettingsAccess &access, EffectDtmf::Settings &settings)
      : DefaultEffectUIValidator{effect, access}
      , mSettings{settings}
   {}
   virtual ~MyValidator() = default;

   Effect &GetEffect() const { return static_cast<Effect&>(mEffect); }

   bool ValidateUI() override;
   bool UpdateUI() override;
   void DoUpdateUI();

   void PopulateOrExchange(ShuttleGui & S,
      const EffectSettings &settings, double duration, double projectRate);
   void OnSequence(wxCommandEvent & evt);
   void OnDuration(wxCommandEvent & evt);
   void OnDutyCycle(wxCommandEvent & evt);

   EffectDtmf::Settings &mSettings;

   wxTextCtrl *mDtmfSequenceT;
   wxSlider   *mDtmfDutyCycleS;
   NumericTextCtrl *mDtmfDurationT;
   wxStaticText *mDtmfToneT;
   wxStaticText *mDtmfSilenceT;
   wxStaticText *mDtmfDutyT;
};

void MyValidator::PopulateOrExchange(ShuttleGui & S,
   const EffectSettings &settings, double duration, double projectRate)
{
   // Hold a reference to special settings, still in the singleton Effect
   // object
   auto &dtmfSettings = mSettings;

   // Do NOT hold a reference to settings, but just use it to find some initial
   // duration values.  (It came from EffectSettingsAccess so its stable address
   // can't be relied on.)

   // dialog will be passed values from effect
   // Effect retrieves values from saved config
   // Dialog will take care of using them to initialize controls
   // If there is a selection, use that duration, otherwise use
   // value from saved config: this is useful is user wants to
   // replace selection with dtmf sequence

   S.AddSpace(0, 5);
   S.StartMultiColumn(2, wxCENTER);
   {
      mDtmfSequenceT =
      S
         .Validator([&dtmfSettings]{
            wxTextValidator vldDtmf(
               wxFILTER_INCLUDE_CHAR_LIST, &dtmfSettings.dtmfSequence);
            vldDtmf.SetIncludes(wxArrayString(WXSIZEOF(kSymbols), kSymbols));
            return vldDtmf;
         })
         .AddTextBox(XXO("DTMF &sequence:"), wxT(""), 10);
      BindTo(*mDtmfSequenceT, wxEVT_TEXT, &MyValidator::OnSequence);

      // A control with no event handler but the validator causes updates
      // when TransferData functions are called
      S
         .Validator<FloatingPointValidator<double>>(
            3, &dtmfSettings.dtmfAmplitude, NumValidatorStyle::NO_TRAILING_ZEROES,
            MIN_Amplitude, MAX_Amplitude)
         .AddTextBox(XXO("&Amplitude (0-1):"), wxT(""), 10);

      S.AddPrompt(XXO("&Duration:"));
      auto &extra = settings.extra;
      mDtmfDurationT = safenew
         NumericTextCtrl(S.GetParent(), wxID_ANY,
                         NumericConverter::TIME,
                         extra.GetDurationFormat(),
                         duration,
                         projectRate,
                         NumericTextCtrl::Options{}
                            .AutoPos(true));
      S.Name(XO("Duration"))
         .AddWindow(mDtmfDurationT);
      BindTo(*mDtmfDurationT, wxEVT_TEXT, &MyValidator::OnDuration);

      S.AddFixedText(XO("&Tone/silence ratio:"), false);
      mDtmfDutyCycleS =
      S
         .Style(wxSL_HORIZONTAL | wxEXPAND)
         .MinSize( { -1, -1 } )
         .AddSlider( {},
                     dtmfSettings.dtmfDutyCycle * SCL_DutyCycle,
                     MAX_DutyCycle * SCL_DutyCycle,
                     MIN_DutyCycle * SCL_DutyCycle);
      BindTo(*mDtmfDutyCycleS, wxEVT_SLIDER, &MyValidator::OnDutyCycle);
   }
   S.EndMultiColumn();

   S.StartMultiColumn(2, wxCENTER);
   {
      S.AddFixedText(XO("Duty cycle:"), false);
      mDtmfDutyT =
      S.AddVariableText(XO("%.1f %%")
         .Format( dtmfSettings.dtmfDutyCycle ), false);
      
      S.AddFixedText(XO("Tone duration:"), false);
      mDtmfSilenceT =
      /* i18n-hint milliseconds */
      S.AddVariableText(XO("%.0f ms")
         .Format( dtmfSettings.dtmfTone * 1000.0 ), false);

      S.AddFixedText(XO("Silence duration:"), false);
      mDtmfToneT =
      /* i18n-hint milliseconds */
      S.AddVariableText(XO("%0.f ms")
         .Format( dtmfSettings.dtmfSilence * 1000.0 ), false);
   }
   S.EndMultiColumn();
}
}

// Effect implementation

std::unique_ptr<EffectUIValidator>
EffectDtmf::PopulateOrExchange(ShuttleGui & S, EffectSettingsAccess &access)
{
   auto &settings = access.Get();
   auto &dtmfSettings = mSettings;
   auto result = std::make_unique<MyValidator>(*this, access, dtmfSettings);
   result->PopulateOrExchange(S, settings, GetDuration(), mProjectRate);
   return result;
}

bool MyValidator::UpdateUI()
{
   auto &dtmfSettings = mSettings;
   mDtmfDutyCycleS->SetValue(dtmfSettings.dtmfDutyCycle * SCL_DutyCycle);

   mDtmfDurationT->SetValue(GetEffect().GetDuration());

   DoUpdateUI();

   return true;
}

bool MyValidator::ValidateUI()
{
   auto &dtmfSettings = mSettings;
   auto &effect = GetEffect();
   dtmfSettings.dtmfDutyCycle =
      (double) mDtmfDutyCycleS->GetValue() / SCL_DutyCycle;
   effect.SetDuration(mDtmfDurationT->GetValue());

   // recalculate to make sure all values are up-to-date. This is especially
   // important if the user did not change any values in the dialog
   dtmfSettings.Recalculate(effect);

   return true;
}

// EffectDtmf implementation

// Updates dtmfNTones, dtmfTone, dtmfSilence, and sometimes duration
// They depend on dtmfSequence, dtmfDutyCycle, and duration
void EffectDtmf::Settings::Recalculate(Effect &effect)
{
   // remember that dtmfDutyCycle is in range (0.0-100.0)

   dtmfNTones = (int) dtmfSequence.length();

   if (dtmfNTones==0) {
      // no tones, all zero: don't do anything
      // this should take care of the case where user got an empty
      // dtmf sequence into the generator: track won't be generated
      effect.SetDuration(0.0);
      dtmfTone = 0;
      dtmfSilence = 0;
   } else {
      if (dtmfNTones==1) {
        // single tone, as long as the sequence
          dtmfTone = effect.GetDuration();
          dtmfSilence = 0;
      } else {
         // Don't be fooled by the fact that you divide the sequence into dtmfNTones:
         // the last slot will only contain a tone, not ending with silence.
         // Given this, the right thing to do is to divide the sequence duration
         // by dtmfNTones tones and (dtmfNTones-1) silences each sized according to the duty
         // cycle: original division was:
         // slot=mDuration / (dtmfNTones*(dtmfDutyCycle/MAX_DutyCycle)+(dtmfNTones-1)*(1.0-dtmfDutyCycle/MAX_DutyCycle))
         // which can be simplified in the one below.
         // Then just take the part that belongs to tone or silence.
         //
         double slot = effect.GetDuration()
            / ((double)dtmfNTones + (dtmfDutyCycle / 100.0) - 1);
         dtmfTone = slot * (dtmfDutyCycle / 100.0); // seconds
         dtmfSilence = slot * (1.0 - (dtmfDutyCycle / 100.0)); // seconds

         // Note that in the extremes we have:
         // - dutyCycle=100%, this means no silence, so each tone will measure mDuration/dtmfNTones
         // - dutyCycle=0%, this means no tones, so each silence slot will measure mDuration/(NTones-1)
         // But we always count:
         // - dtmfNTones tones
         // - dtmfNTones-1 silences
      }
   }
}

bool EffectDtmf::MakeDtmfTone(float *buffer, size_t len, float fs, wxChar tone, sampleCount last, sampleCount total, float amplitude)
{
/*
  --------------------------------------------
              1209 Hz 1336 Hz 1477 Hz 1633 Hz

                          ABC     DEF
   697 Hz          1       2       3       A

                  GHI     JKL     MNO
   770 Hz          4       5       6       B

                  PQRS     TUV     WXYZ
   852 Hz          7       8       9       C

                          oper
   941 Hz          *       0       #       D
  --------------------------------------------
  Essentially we need to generate two sin with
  frequencies according to this table, and sum
  them up.
  sin wave is generated by:
   s(n)=sin(2*pi*n*f/fs)

  We will precalculate:
     A= 2*pi*f1/fs
     B= 2*pi*f2/fs

  And use two switch statements to select the frequency

  Note: added support for letters, like those on the keypad
        This support is only for lowercase letters: uppercase
        are still considered to be the 'military'/carrier extra
        tones.
*/

   float f1, f2=0.0;
   double A,B;

   // select low tone: left column
   switch (tone) {
      case '1':   case '2':   case '3':   case 'A':
      case 'a':   case 'b':   case 'c':
      case 'd':   case 'e':   case 'f':
         f1=697;
         break;
      case '4':   case '5':   case '6':   case 'B':
      case 'g':   case 'h':   case 'i':
      case 'j':   case 'k':   case 'l':
      case 'm':   case 'n':   case 'o':
         f1=770;
         break;
      case '7':   case '8':   case '9':   case 'C':
      case 'p':   case 'q':   case 'r':   case 's':
      case 't':   case 'u':   case 'v':
      case 'w':   case 'x':   case 'y':   case 'z':
         f1=852;
         break;
      case '*':   case '0':   case '#':   case 'D':
         f1=941;
         break;
      default:
         f1=0;
   }

   // select high tone: top row
   switch (tone) {
      case '1':   case '4':   case '7':   case '*':
      case 'g':   case 'h':   case 'i':
      case 'p':   case 'q':   case 'r':   case 's':
         f2=1209;
         break;
      case '2':   case '5':   case '8':   case '0':
      case 'a':   case 'b':   case 'c':
      case 'j':   case 'k':   case 'l':
      case 't':   case 'u':   case 'v':
         f2=1336;
         break;
      case '3':   case '6':   case '9':   case '#':
      case 'd':   case 'e':   case 'f':
      case 'm':   case 'n':   case 'o':
      case 'w':   case 'x':   case 'y':   case 'z':
         f2=1477;
         break;
      case 'A':   case 'B':   case 'C':   case 'D':
         f2=1633;
         break;
      default:
         f2=0;
   }

   // precalculations
   A=B=2*M_PI/fs;
   A*=f1;
   B*=f2;

   // now generate the wave: 'last' is used to avoid phase errors
   // when inside the inner for loop of the Process() function.
   for(decltype(len) i = 0; i < len; i++) {
      buffer[i] = amplitude * 0.5 *
         (sin( A * (i + last).as_double() ) +
          sin( B * (i + last).as_double() ));
   }

   // generate a fade-in of duration 1/250th of second
   if (last == 0) {
      A = wxMin(len, (fs / kFadeInOut));
      for(size_t i = 0; i < A; i++) {
         buffer[i] *= i/A;
      }
   }

   // generate a fade-out of duration 1/250th of second
   if (last >= total - len) {
      // we are at the last buffer of 'len' size, so, offset is to
      // backup 'A' samples, from 'len'
      A = wxMin(len, (fs / kFadeInOut));
      size_t offset = len - A;
      wxASSERT(offset >= 0);
      for(size_t i = 0; i < A; i++) {
         buffer[i + offset] *= (1 - (i / A));
      }
   }
   return true;
}

void MyValidator::DoUpdateUI()
{
   // Update some texts in response to controls
   auto &dtmfSettings = mSettings;

   mDtmfDutyT
      ->SetLabel(wxString::Format(wxT("%.1f %%"), dtmfSettings.dtmfDutyCycle));
   mDtmfDutyT->SetName(mDtmfDutyT->GetLabel()); // fix for bug 577 (NVDA/Narrator screen readers do not read static text in dialogs)

   mDtmfSilenceT
      ->SetLabel(wxString::Format(_("%.0f ms"), dtmfSettings.dtmfTone * 1000.0));
   mDtmfSilenceT->SetName(mDtmfSilenceT->GetLabel()); // fix for bug 577 (NVDA/Narrator screen readers do not read static text in dialogs)

   mDtmfToneT
      ->SetLabel(wxString::Format(_("%.0f ms"), dtmfSettings.dtmfSilence * 1000.0));
   mDtmfToneT->SetName(mDtmfToneT->GetLabel()); // fix for bug 577 (NVDA/Narrator screen readers do not read static text in dialogs)
}

void MyValidator::OnSequence(wxCommandEvent & WXUNUSED(evt))
{
   auto &dtmfSettings = mSettings;
   dtmfSettings.dtmfSequence = mDtmfSequenceT->GetValue();
   dtmfSettings.Recalculate(GetEffect());
   DoUpdateUI();
}

void MyValidator::OnDuration(wxCommandEvent & WXUNUSED(evt))
{
   auto &dtmfSettings = mSettings;
   auto &effect = GetEffect();
   effect.SetDuration(mDtmfDurationT->GetValue());
   dtmfSettings.Recalculate(effect);
   DoUpdateUI();
}

void MyValidator::OnDutyCycle(wxCommandEvent & evt)
{
   auto &dtmfSettings = mSettings;
   dtmfSettings.dtmfDutyCycle = (double) evt.GetInt() / SCL_DutyCycle;
   dtmfSettings.Recalculate(GetEffect());
   DoUpdateUI();
}
